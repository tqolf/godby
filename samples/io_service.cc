#include "io_service.h"

namespace godby::co
{
thread_local unsigned int scheduler::M_thread_id = 0;
thread_local unsigned int scheduler::M_coro_scheduler_id = 0;
unsigned int scheduler::M_coro_scheduler_count = 0;

scheduler::scheduler()
{
	M_id = ++M_coro_scheduler_count;
	M_thread_cxts.reserve(128);
	thread_context *io_cxt = new thread_context;
	io_cxt->M_tasks = new task_queue(64);
	M_thread_cxts.push_back(io_cxt);
	spawn_workers(std::thread::hardware_concurrency());
}

scheduler::~scheduler()
{
	M_stop_requested = true;
	M_task_wait_flag.test_and_set(std::memory_order_relaxed);
	M_task_wait_flag.notify_all();
	for (auto &t_cxt : this->M_thread_cxts) {
		if (t_cxt->M_thread.joinable()) { t_cxt->M_thread.join(); }
	}
}

bool scheduler::steal_task(std::coroutine_handle<> &handle) noexcept
{
	auto total_threads = M_total_threads.load(std::memory_order_relaxed);

	bool c = false;
	do {
		c = false;
		unsigned int i = (M_thread_id + 1) % (total_threads + 1);
		do {
			task_queue *queue = M_thread_cxts[i]->M_tasks;
			if (queue->steal(handle)) { return true; }
			c = c | !queue->empty();
			i = (i + 1) % (total_threads + 1);
		} while (i != M_thread_id);
	} while (c);

	return false;
}

void scheduler::schedule(const std::coroutine_handle<> &handle) noexcept
{
	if (!M_thread_id | (M_coro_scheduler_id != M_id)) {
		std::unique_lock lk(M_global_task_queue_mutex);
		M_thread_cxts[0]->M_tasks->enqueue(handle);
	} else {
		M_thread_cxts[M_thread_id]->M_tasks->enqueue(handle);
	}
	if (!M_task_wait_flag.test_and_set(std::memory_order_relaxed)) { M_task_wait_flag.notify_one(); }
}

bool scheduler::peek_next_coroutine(std::coroutine_handle<> &handle) noexcept
{
	return M_thread_cxts[M_thread_id]->M_tasks->dequeue(handle) ? true : steal_task(handle);
}

std::coroutine_handle<> scheduler::get_waiting_channel() noexcept
{
	return M_thread_cxts[M_thread_id]->M_waiting_channel;
}

auto scheduler::get_next_coroutine() noexcept -> std::coroutine_handle<>
{
	std::coroutine_handle<> handle;
	return peek_next_coroutine(handle) ? handle : get_waiting_channel();
}

scheduler_task scheduler::awaiter()
{
	struct thread_awaiter {
		std::coroutine_handle<> &M_handle_ref;

		constexpr bool await_ready() const noexcept
		{
			return false;
		}

		auto await_suspend(const std::coroutine_handle<> &) const noexcept
		{
			return M_handle_ref;
		}

		constexpr void await_resume() const noexcept {}
	};

	while (!M_stop_requested) {
		std::coroutine_handle<> handle;

		while (!peek_next_coroutine(handle)) {
			std::unique_lock<std::mutex> lk(M_task_mutex);
			M_task_wait_flag.wait(false, std::memory_order_relaxed);
			lk.unlock();
			if (M_stop_requested) [[unlikely]] { co_return; }
			M_task_wait_flag.clear(std::memory_order_relaxed);
		}

		co_await thread_awaiter{handle};
	}
}

void scheduler::spawn_workers(const unsigned int &count)
{
	std::unique_lock<std::mutex> lk(M_spawn_thread_mutex);
	for (unsigned int i = 0; i < count; ++i) {
		init_thread();
		M_thread_cxts[M_total_threads]->M_thread = std::thread([&](unsigned int id) {
			M_thread_id = id;
			M_coro_scheduler_id = M_id;
			M_thread_cxts[id]->M_waiting_channel.resume();
		}, ++M_total_threads);
	}
}

void scheduler::init_thread()
{
	thread_context *cxt = new thread_context;
	cxt->M_thread_status.M_status = thread_status::STATUS::READY;
	cxt->M_tasks = new task_queue(64);
	cxt->M_waiting_channel = awaiter().handle();
	M_thread_cxts.push_back(cxt);
}

void scheduler::set_thread_status(thread_status::STATUS status) noexcept
{
	switch (status) {
		case thread_status::STATUS::READY:
			set_thread_ready();
			break;
		case thread_status::STATUS::RUNNING:
			set_thread_running();
			break;
		case thread_status::STATUS::SUSPENDED:
			set_thread_suspended();
			break;
		default:
			break;
	}
}

void scheduler::set_thread_suspended() noexcept
{
	this->M_total_running_threads.fetch_sub(1, std::memory_order_relaxed);
	if ((this->M_total_suspended_threads.fetch_add(1, std::memory_order_relaxed) + 1) >= M_total_threads.load(std::memory_order_relaxed)) { spawn_workers(1); }
	M_thread_cxts[M_thread_id]->M_thread_status.M_status.store(thread_status::STATUS::SUSPENDED, std::memory_order_relaxed);
}

void scheduler::set_thread_ready() noexcept
{
	this->M_total_running_threads.fetch_sub(1, std::memory_order_relaxed);
	this->M_total_ready_threads.fetch_add(1, std::memory_order_relaxed);
	M_thread_cxts[M_thread_id]->M_thread_status.M_status.store(thread_status::STATUS::READY, std::memory_order_relaxed);
}

void scheduler::set_thread_running() noexcept
{
	if (M_thread_cxts[M_thread_id]->M_thread_status.M_status.load(std::memory_order_relaxed) == thread_status::STATUS::SUSPENDED) {
		this->M_total_suspended_threads.fetch_sub(1, std::memory_order_relaxed);
	} else {
		this->M_total_ready_threads.fetch_sub(1, std::memory_order_relaxed);
	}
	this->M_total_running_threads.fetch_add(1, std::memory_order_relaxed);
	M_thread_cxts[M_thread_id]->M_thread_status.M_status.store(thread_status::STATUS::RUNNING, std::memory_order_relaxed);
}

thread_local unsigned int io_service::M_thread_id = 0;
thread_local uring_data::allocator *io_service::M_uio_data_allocator = nullptr;
thread_local io_op_pipeline *io_service::M_io_queue = nullptr;

io_service::io_service(const u_int &entries, const u_int &flags) : io_operation(this), M_entries(entries), M_flags(flags)
{
	io_uring_queue_init(entries, &M_uring, flags);
	M_io_cq_thread = std::move(std::thread([&] { this->io_loop(); }));
}

io_service::io_service(const u_int &entries, io_uring_params &params) : io_operation(this), M_entries(entries)
{
	io_uring_queue_init_params(entries, &M_uring, &params);
	M_io_cq_thread = std::move(std::thread([&] { this->io_loop(); }));
}

io_service::~io_service()
{
	M_stop_requested.store(true, std::memory_order_relaxed);
	nop(IOSQE_IO_DRAIN);
	M_io_cq_thread.join();
}

void io_service::io_loop() noexcept
{
	while (!M_stop_requested.load(std::memory_order_relaxed)) {
		io_uring_cqe *cqe = nullptr;
		if (io_uring_wait_cqe(&M_uring, &cqe) == 0) {
			handle_completion(cqe);
			io_uring_cqe_seen(&M_uring, cqe);
		} else {
			std::cerr << "Wait CQE Failed\n";
		}
		unsigned completed = 0;
		unsigned head;

		io_uring_for_each_cqe(&M_uring, head, cqe)
		{
			++completed;
			handle_completion(cqe);
		}
		if (completed) { io_uring_cq_advance(&M_uring, completed); }
		if (M_io_queue_overflow.load(std::memory_order_relaxed) != nullptr) [[unlikely]] { submit(); }
	}
	io_uring_queue_exit(&M_uring);
	return;
}

bool io_service::io_queue_empty() const noexcept
{
	bool is_empty = true;
	for (auto &q : this->M_io_queues) { is_empty = is_empty & q->empty(); }
	return is_empty;
}

void io_service::setup_thread_context()
{
	if (M_thread_id == 0) {
		M_thread_id = M_threads.fetch_add(1, std::memory_order_relaxed) + 1;
		M_uio_data_allocator = new uring_data::allocator;
		M_uio_data_allocators.push_back(M_uio_data_allocator);
		M_io_queue = new io_op_pipeline(128);
		M_io_queues.push_back(M_io_queue);
	}
}

void io_service::submit()
{
	while (!io_queue_empty() && !M_io_sq_running.exchange(true, std::memory_order_seq_cst)) {
		auto overflow_queue = M_io_queue_overflow.load(std::memory_order_relaxed);
		if (overflow_queue != nullptr) [[unlikely]] {
			int res = overflow_queue->init_io_uring_ops(&M_uring);
			if (res >= 0) {
				M_io_queue_overflow.store(nullptr, std::memory_order_relaxed);
			} else {
				return;
			}
		}

		unsigned int completed = 0;
		while (!io_queue_empty()) {
			for (auto &q : M_io_queues) {
				int res = q->init_io_uring_ops(&M_uring);
				if (res < 0) [[unlikely]] {
					M_io_queue_overflow.store(q, std::memory_order_relaxed);
					return;
				} else {
					completed += res;
				}
			}
		}
		if (completed) { io_uring_submit(&M_uring); }
		M_io_sq_running.store(false, std::memory_order_relaxed);
	}
}

void io_service::handle_completion(io_uring_cqe *cqe)
{
	auto data = static_cast<uring_data *>(io_uring_cqe_get_data(cqe));
	if (data != nullptr) {
		data->M_result = cqe->res;
		data->M_flags = cqe->flags;
		if (data->M_handle_ctl.exchange(true, std::memory_order_acq_rel)) { data->M_scheduler->schedule(data->M_handle); }
		if (data->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { data->destroy(); }
	}
}

void io_service::submit(io_batch<io_service> &batch)
{
	M_io_queue->enqueue(batch.operations());
	submit();
}

void io_service::submit(io_link<io_service> &io_link)
{
	auto &operations = io_link.operations();
	size_t op_count = operations.size() - 1;

	for (size_t i = 0; i < op_count; ++i) {
		std::visit([](auto &&op) { op.M_sqe_flags |= IOSQE_IO_HARDLINK; }, operations[i]);
	}

	M_io_queue->enqueue(operations);

	submit();
}

async<void> timer(io_service *io, itimerspec spec, std::function<void()> func)
{
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(tfd, 0, &spec, NULL);
	std::stop_token st = co_await get_stop_token();
	std::stop_callback scb(st, [&] { io->close(tfd); });

	while (!st.stop_requested()) {
		std::cerr << "Waiting for timer to trigger\n";
		unsigned long u;
		co_await io->read(tfd, &u, sizeof(u), 0);
		func();
	}
	co_await io->close(tfd);
}

async<> delay(io_service *io, unsigned long sec, unsigned long nsec)
{
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	itimerspec spec;
	spec.it_value.tv_sec = sec;
	spec.it_value.tv_nsec = nsec;
	spec.it_interval.tv_sec = 0;
	spec.it_interval.tv_nsec = 0;
	timerfd_settime(tfd, 0, &spec, NULL);

	std::stop_token st = co_await get_stop_token();
	std::stop_callback scb(st, [&] { io->close(tfd); });

	unsigned long u;
	co_await io->read(tfd, &u, sizeof(u), 0);
	co_await io->close(tfd);
}
} // namespace godby::co


// examples
#include <fcntl.h>
#include <string.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/time_types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/time_types.h>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace godby::co;

namespace example1
{
async<int> print_file(io_service &io, int dir, const std::string filename)
{
	char buffer[256]{0};
	int fd = co_await io.openat(dir, filename.c_str(), 0, 0);
	co_return co_await io.read(fd, buffer, 256, 0);
}

launch<int> coroutine_1(io_service &io, int dir)
{
	auto scheduler = co_await get_scheduler();
	auto print_file_task = print_file(io, dir, "log");
	co_return co_await print_file_task.schedule_on(scheduler);
}

launch<int> coroutine_2(io_service &io, int dir)
{
	co_return co_await print_file(io, dir, "response");
}
} // namespace example1

namespace example2
{
using io = io_service;

async<int> loop(io *io, int socket_fd)
{
	auto st = co_await get_stop_token();
	while (!st.stop_requested()) {
		sockaddr_in client_addr;
		socklen_t client_length;
		auto client_awaiter = io->accept(socket_fd, (sockaddr *)&client_addr, &client_length, 0);
		std::stop_callback sr(st, [&] {
			std::cerr << "Stop Requested\n";
			return io->cancel(client_awaiter, 0);
		});
		std::cerr << "Waiting for request\n";
		int fd = co_await client_awaiter;
		if (fd <= 0) { std::cerr << "IO Request cancelled\n"; }
		std::cerr << "Wait 5 sec before exiting\n";
		sleep(5);
	}
	co_return 1;
}

async<void> waiter(auto &awaiter)
{
	std::cerr << "Waiter() Enter\n";
	co_await awaiter;
	std::cerr << "Waiter() Exit\n";
}

async<void> cancelable(io *io, int socket_fd)
{
	auto loop1 = loop(io, socket_fd).schedule_on(co_await get_scheduler());
	auto w = waiter(loop1).schedule_on(co_await get_scheduler());
	std::cerr << "Sleepint for 5 sec\n";
	sleep(5);
	std::cerr << "Requesting cancel\n";
	co_await loop1.cancel();
	std::cerr << "loop canceled\n";
	co_await w;
}

launch<> start_accept(io *io)
{
	int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	const int val = 1;
	int ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	if (ret == -1) {
		std::cerr << "setsockopt() set_reuse_address\n";
		co_return;
	}

	sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(8080);
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(socket_fd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		std::cerr << "Error binding socket\n";
		co_return;
	}
	if (listen(socket_fd, 240) < 0) {
		std::cerr << "Error listening\n";
		co_return;
	}

	co_await cancelable(io, socket_fd);
	co_return;
}
} // namespace example2

namespace example3
{
async<void> print_task(event &e)
{
	for (int i = 0; i < 10; ++i) {
		sleep(1);
		e.set(i);
	}
	co_return;
}

launch<int> launch_coroutine()
{
	event e;
	auto t = print_task(e).schedule_on(co_await get_scheduler());
	for (int i = 0; i < 10; ++i) { std::cout << co_await e << std::endl; }
	co_await t;
	co_return 0;
}
} // namespace example3

namespace example4
{
generator<int> integers()
{
	auto st = co_await get_stop_token();
	int i = 0;
	while (!st.stop_requested()) {
		sleep(1);
		co_yield i++;
	}
}

launch<int> launch_coroutine()
{
	auto ints = integers();
	while (ints) {
		int val = co_await ints;
		std::cout << val << std::endl;
		if (val == 10) { co_await ints.cancel(); }
	}
	co_return 1;
}
} // namespace example4

namespace example5
{
using io = io_service;

char *send_buffer;
char *read_buffer;

size_t sb_len;
size_t rb_len;

int get_tcp_socket()
{
	int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	const int val = 1;
	int ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	if (ret == -1) {
		std::cerr << "setsockopt() set_reuse_address\n";
		return -1;
	}

	sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(8080);
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(socket_fd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		std::cerr << "Error binding socket\n";
		return -1;
	}
	if (listen(socket_fd, 240) < 0) {
		std::cerr << "Error listening\n";
		return -1;
	}
	return socket_fd;
}

task<> fill_response_from_file(io *io)
{
	int dfd = open(".", 0);
	int fd = co_await io->openat(dfd, "response", 0, 0);
	sb_len = co_await io->read_fixed(fd, send_buffer, 1024, 0, 0);
	co_await io->close(fd);
}

async<> handle_client(int fd, io *io)
{
	while (true) {
		auto r = co_await io->read_fixed(fd, read_buffer, rb_len, 0, 1);
		if (r <= 0) {
			co_await io->close(fd);
			co_return;
		}
		co_await io->write_fixed(fd, send_buffer, sb_len, 0, 0);
	}

	co_return;
}

// This coroutine will suspend untill a connection is received and yield the
// connection fd.
generator<int> get_connections(io *io, int socket_fd)
{
	// Get a stop_token for this coroutine.
	auto st = co_await get_stop_token();
	while (!st.stop_requested()) { // Run untill a cancel is requested.
		sockaddr_in client_addr;
		socklen_t client_length;

		// Submit an io request for accepting a connection.
		auto awaiter = io->accept(socket_fd, (sockaddr *)&client_addr, &client_length, 0);

		// Register a stop_callback for this coroutine.
		std::stop_callback sc(st, [&] { io->cancel(awaiter, 0); });

		// Suspend this coroutine till a connection is received and yield the
		// connection fd.
		co_yield co_await awaiter;
	}
	co_yield 0;
}

async<> server(io *io)
{
	int socket_fd = get_tcp_socket();
	co_await fill_response_from_file(io);

	// Get a connection generator
	auto connections = get_connections(io, socket_fd);

	// Cancel the get_connection coroutine if this coroutine receives cancelation.
	std::stop_callback cb(co_await get_stop_token(), [&] { connections.cancel(); });

	// Run untill get_connection coroutine is stoped.
	while (connections) {
		// Get a connection
		auto fd = co_await connections;
		if (fd <= 0) { co_return; }

		// Handle the client in another coroutine asynchronously
		handle_client(fd, io).schedule_on(co_await get_scheduler());
	}
	co_return;
}

launch<> start_accept(io *io)
{
	auto s = server(io).schedule_on(co_await get_scheduler());
	sleep(2);
	std::cout << "Press Enter to stop\n";
	std::cin.get();
	co_await s.cancel();
}
} // namespace example5

namespace example6
{
using io = io_service;

async<> wait_for_connection(auto &awaiter)
{
	std::cout << "Waiting for connection\n";
	co_await awaiter;
	std::cerr << "Request Cancelled\n";
	co_return;
}

async<> loop(io *io, int socket_fd)
{
	while (true) {
		sockaddr_in client_addr;
		socklen_t client_length;
		auto client_awaiter = io->accept(socket_fd, (sockaddr *)&client_addr, &client_length, 0);
		wait_for_connection(client_awaiter).schedule_on(co_await get_scheduler());
		std::cerr << "Sleeping for 5 sec\n";
		sleep(5);
		co_await io->cancel(client_awaiter, 0);
	}
}

launch<> start_accept(io *io)
{
	int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	const int val = 1;
	int ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
	if (ret == -1) {
		std::cerr << "setsockopt() set_reuse_address\n";
		co_return;
	}

	sockaddr_in serv_addr;
	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(8080);
	serv_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(socket_fd, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
		std::cerr << "Error binding socket\n";
		co_return;
	}
	if (listen(socket_fd, 240) < 0) {
		std::cerr << "Error listening\n";
		co_return;
	}

	co_await loop(io, socket_fd);
	co_return;
}
} // namespace example6

namespace example7
{
task<void> print_task()
{
	std::cout << "print_task()\n";
	co_return;
}

launch<> launch_coroutine(io_service *io)
{
	std::cout << "Launch Coroutine\n";

	int tfd = timerfd_create(CLOCK_REALTIME, 0);

	itimerspec spec{{5, 0}, {5, 0}};
	timerfd_settime(tfd, 0, &spec, NULL);

	std::cerr << "Going to wait on timer_fd\n";
	co_await io->poll_add(tfd, POLL_IN);
	std::cerr << "Poll Event received\n";

	co_await io->delay(5, 0);
}
} // namespace example7

namespace example8
{
launch<> hello()
{
	std::cout << "Hello Coroutine\n";
	co_return;
}

launch<int> add(int a, int b)
{
	co_return a + b;
}
} // namespace example8

namespace example9
{
task<int> print_file(io_service &io)
{
	char buffer1[128]{0};
	char buffer2[256]{0};
	int dir = open(".", 0);
	auto batch_1 = io.batch();
	auto fd1_awaiter = batch_1.openat(dir, "log", 0, 0);
	auto fd2_awaiter = batch_1.openat(dir, "response", 0, 0);
	io.submit(batch_1);

	int fd1 = co_await fd1_awaiter;
	int fd2 = co_await fd2_awaiter;

	auto link1 = io.link();
	// Wait for 5 sec

	link1.delay(5, 0);
	auto rd1_awaiter = link1.read(fd1, buffer1, 128, 0);
	link1.close(fd1);
	auto rd2_awaiter = link1.read(fd2, buffer2, 256, 0);
	link1.close(fd2);
	io.submit(link1);

	int len1 = co_await rd1_awaiter;
	int len2 = co_await rd2_awaiter;

	std::cout << buffer1 << std::endl;
	std::cout << buffer2 << std::endl;
	co_return len1 + len2;
}

launch<int> coroutine_1(io_service &io)
{
	auto print_file_task = print_file(io);
	co_return co_await print_file_task;
}
} // namespace example9

namespace example10
{
task<int> print_file(io_service &io)
{
	char buffer1[128]{0};
	char buffer2[256]{0};
	int dir = open(".", 0);
	auto batch_1 = io.batch();
	auto fd1_awaiter = batch_1.openat(dir, "log", 0, 0);
	auto fd2_awaiter = batch_1.openat(dir, "response", 0, 0);
	io.submit(batch_1);

	int fd1 = co_await fd1_awaiter;
	int fd2 = co_await fd2_awaiter;

	auto batch_2 = io.batch();
	auto rd1_awaiter = batch_2.read(fd1, buffer1, 128, 0);
	auto rd2_awaiter = batch_2.read(fd2, buffer2, 256, 0);
	io.submit(batch_2);

	int len1 = co_await rd1_awaiter;
	int len2 = co_await rd2_awaiter;

	std::cout << buffer1 << std::endl;
	std::cout << buffer2 << std::endl;
	co_return len1 + len2;
}

launch<int> coroutine_1(io_service &io)
{
	auto print_file_task = print_file(io);
	co_return co_await print_file_task;
}
} // namespace example10

namespace example11
{
task<int> print_file(io_service &io, const std::string &filename)
{
	char buffer[128]{0};
	int dir = open(".", 0);
	int fd = co_await io.openat(dir, filename.c_str(), 0, 0);
	auto t1 = io.read(fd, buffer, 128, 0);
	int len = co_await t1;
	std::cout << buffer << std::endl;
	co_return len;
}

launch<int> coroutine_1(io_service &io)
{
	auto print_file_task = print_file(io, "log");
	co_return co_await print_file_task;
}
} // namespace example11

namespace example12
{
task<void> print_task()
{
	std::cout << "print_task()\n";
	co_return;
}

launch<> launch_coroutine()
{
	std::cout << "Launch Coroutine\n";
	co_await print_task();
}

task<int> add_number(int a, int b)
{
	std::cout << "add a+b\n";
	co_return a + b;
}

launch<int> launch_coroutine_add()
{
	std::cout << "Launch Coroutine Add\n";
	co_return co_await add_number(10, 20);
}
} // namespace example12

namespace example13
{
async<int> print()
{
	std::cout << "Print\n";
	co_return 2;
}

launch<> coroutine_2(io_service *io)
{
	std::cerr << "Calling delayed\n";
	int a = co_await delayed<int>(io, 5, 0, [] {
		std::cerr << "Run delayed task\n";
		return 101;
	});
	std::cout << a << std::endl;

	co_await delayed(io, 5, 0, print());

	auto t = timer(io, {{1, 0}, {5, 0}}, []() -> async<> {
		std::cerr << "Hello World\n";
		co_return;
	});

	co_await t;
}
} // namespace example13

int main(int argc, char **argv)
{
	{
		using namespace example1;
		scheduler scheduler;
		io_service io(100, 0);

		int dir = open(".", 0);

		auto cr1 = coroutine_1(io, dir).schedule_on(&scheduler);
		auto cr2 = coroutine_2(io, dir).schedule_on(&scheduler);

		std::cout << "File Length 1 : " << cr1 << std::endl;
		std::cout << "File Length 2 : " << cr2 << std::endl;
	}

	{
		using namespace example2;

		scheduler scheduler;
		io io(1000, 0);
		if (argc == 2) { scheduler.spawn_workers(atoi(argv[1])); }

		start_accept(&io).schedule_on(&scheduler).join();
	}

	{
		using namespace example3;
		scheduler schd;
		int val = launch_coroutine().schedule_on(&schd);
	}

	{
		using namespace example4;

		scheduler schd;
		int a = launch_coroutine().schedule_on(&schd);
	}

	{
		using namespace example5;

		scheduler scheduler;
		io io(1000, 0);

		rb_len = 1024;
		send_buffer = new char[1024];
		read_buffer = new char[rb_len];

		iovec vec[2];
		vec[0].iov_base = send_buffer;
		vec[0].iov_len = 1024;
		vec[1].iov_base = read_buffer;
		vec[1].iov_len = 1024;

		if (!io.register_buffer(vec)) { std::cerr << "Buffer Registeration failed\n"; }

		start_accept(&io).schedule_on(&scheduler).join();
	}

	{
		using namespace example6;

		scheduler scheduler;
		io io(1000, 0);
		if (argc == 2) { scheduler.spawn_workers(atoi(argv[1])); }

		start_accept(&io).schedule_on(&scheduler).join();
	}

	{
		using namespace example8;

		scheduler schd;

		auto cr1 = hello().schedule_on(&schd); // Run coroutine on the scheduler
		cr1.join();							   // Wait for the coroutine finish

		// When returning a value we can use convertion operator to wait and get the
		// value
		int cr2 = add(10, 20).schedule_on(&schd);
		std::cout << cr2 << std::endl;
	}

	{
		using namespace example7;

		scheduler schd;
		io_service io(100, 0);

		launch_coroutine(&io).schedule_on(&schd).join();
	}

	{
		using namespace example9;

		scheduler scheduler;
		io_service io(100, 0);
		int len = coroutine_1(io).schedule_on(&scheduler);
		std::cout << "File Length : " << len << std::endl;
	}

	{
		using namespace example10;

		scheduler scheduler;
		io_service io(100, 0);
		//   int buffer_size[]{32, 64, 128, 256, 512};
		//   io.create_fixed_buffer(buffer_size);
		int len = coroutine_1(io).schedule_on(&scheduler);
		std::cout << "File Length : " << len << std::endl;
	}

	{
		using namespace example11;

		scheduler scheduler;
		io_service io(100, 0);
		int len = coroutine_1(io).schedule_on(&scheduler);
		std::cout << "File Length : " << len << std::endl;
	}

	{
		using namespace example12;

		scheduler schd;

		launch_coroutine().schedule_on(&schd);
		auto cr = launch_coroutine_add().schedule_on(&schd);
		sleep(1);
		std::cout << cr << std::endl;
		sleep(2);
	}

	{
		using namespace example13;

		scheduler scheduler;
		io_service io(100, 0);

		coroutine_2(&io).schedule_on(&scheduler).join();
	}

	return 0;
}
