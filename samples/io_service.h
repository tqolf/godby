#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <functional>
#include <future>
#include <iostream>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <memory>
#include <mutex>
#include <stop_token>
#include <sys/timerfd.h>
#include <thread>
#include <type_traits>
#include <variant>
#include <vector>

namespace godby::co
{
template <typename T>
class circular_array {
	size_t M_capacity;
	size_t M_mask;
	std::atomic<T> *M_data = nullptr;

  public:
	explicit circular_array(size_t capacity) : M_capacity{capacity}, M_mask{M_capacity - 1}, M_data{new std::atomic<T>[M_capacity]} {}

	~circular_array()
	{
		delete[] M_data;
	}

	template <typename D>
	void push(size_t i, D &&data) noexcept
	{
		M_data[i & M_mask].store(std::forward<D>(data), std::memory_order_relaxed);
	}

	T pop(size_t i) const noexcept
	{
		return M_data[i & M_mask].load(std::memory_order_relaxed);
	}

	circular_array *resize(size_t back, size_t front)
	{
		circular_array *ptr = new circular_array(2 * M_capacity);
		for (size_t i = front; i != back; ++i) { ptr->push(i, pop(i)); }
		return ptr;
	}

	size_t size() const noexcept
	{
		return M_capacity;
	}
};

/* A first in first out lock free data structure
 */

template <typename T>
class io_work_queue {
	// Front of the queue
	std::atomic<size_t> M_front;

	// Back of the queue
	std::atomic<size_t> M_back;

	// Current buffer used for storing data
	std::atomic<circular_array<T> *> M_data{nullptr};

	/* Old buffer that was used for store data.
	 * Keep the old one just in-case some thread is dequeuing when new buffer is
	 * being created.
	 */
	std::atomic<circular_array<T> *> M_old{nullptr};

  public:
	// Create a io_work_queue with size capacity (capacity should be power of 2)
	explicit io_work_queue(size_t capacity)
	{
		M_front.store(0, std::memory_order_relaxed);
		M_back.store(0, std::memory_order_relaxed);
		M_data.store(new circular_array<T>(capacity), std::memory_order_relaxed);
	}

	~io_work_queue()
	{
		delete M_data.load(std::memory_order_relaxed);
		delete M_old.load(std::memory_order_relaxed);
	}

	// Check the io_work_queue is empty.
	bool empty() const noexcept
	{
		size_t back = M_back.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_relaxed);
		return back == front;
	}

	/* Add a new item to back of the queue. If the queue is full expand the queue
	 * to next power of 2
	 */
	void enqueue(T &&item)
	{
		size_t back = M_back.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_acquire);
		circular_array<T> *data = M_data.load(std::memory_order_relaxed);

		// Check the queue is full and resize the array.
		if (data->size() - 1 < static_cast<size_t>(back - front)) [[unlikely]] {
			if (back < front) {
				size_t a = 0;
				auto size = (back + ((a - 1) - front)) + 1;
				if (data->size() - 1 < size) { data = resize(data, back, front); }
			} else {
				data = resize(data, back, front);
			}
		}

		data->push(back, std::forward<T>(item));
		M_back.store(back + 1, std::memory_order_release);
	}

	/* Add a new items to back of the queue. If the queue is full expand the queue
	 * to next power of 2
	 */
	void bulk_enqueue(std::vector<T> &items)
	{
		size_t items_count = items.size();
		size_t back = M_back.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_acquire);
		circular_array<T> *data = M_data.load(std::memory_order_relaxed);

		if ((data->size() - 1 - items_count) < static_cast<size_t>(back - front)) [[unlikely]] {
			if (back < front) {
				size_t a = 0;
				auto size = (back + ((a - 1) - front)) + 1;
				if ((data->size() - 1 - items_count) < size) { data = resize(data, back, front); }
			} else {
				data = resize(data, back, front);
			}
		}
		for (size_t i = 0; i < items_count; ++i) { data->push(back + i, items[i]); }
		M_back.store(back + items_count, std::memory_order_release);
	}

	/* Pop an item from front of the queue.
	 */
	bool dequeue(T &item)
	{
		size_t back = M_back.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_relaxed);

		if (back == front) { return false; }

		circular_array<T> *data = M_data.load(std::memory_order_consume);
		item = data->pop(front);
		M_front.store(M_front + 1, std::memory_order_seq_cst);
		return true;
	}

  protected:
	circular_array<T> *resize(circular_array<T> *data, size_t back, size_t front)
	{
		circular_array<T> *new_data = data->resize(back, front);
		delete M_old.load(std::memory_order_relaxed);
		M_old.store(data, std::memory_order_relaxed);
		M_data.store(new_data, std::memory_order_relaxed);
		return new_data;
	}
};

template <typename T>
class work_stealing_queue {
	std::atomic<size_t> M_front;
	std::atomic<size_t> M_back;
	std::atomic<circular_array<T> *> M_data{nullptr};
	std::atomic<circular_array<T> *> M_old{nullptr};

  public:
	explicit work_stealing_queue(size_t capacity)
	{
		M_front.store(0, std::memory_order_relaxed);
		M_back.store(0, std::memory_order_relaxed);
		M_data.store(new circular_array<T>(capacity), std::memory_order_relaxed);
	}

	~work_stealing_queue()
	{
		delete M_data.load(std::memory_order_relaxed);
	}

	bool empty() const noexcept
	{
		size_t back = M_back.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_relaxed);
		return back == front;
	}

	void enqueue(const T &item)
	{
		size_t back = M_back.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_acquire);
		circular_array<T> *data = M_data.load(std::memory_order_relaxed);

		if (data->size() - 1 < static_cast<size_t>(back - front)) [[unlikely]] {
			if (front - 1 != back) {
				if (back < (front - 1)) {
					size_t a = 0;
					auto size = (back + ((a - 1) - front)) + 1;
					if (data->size() - 1 < size) { data = resize(data, back, front); }
				} else {
					data = resize(data, back, front);
				}
			}
		}

		data->push(back, item);
		M_back.store(back + 1, std::memory_order_release);
	}

	bool dequeue(T &item)
	{
		size_t back = M_back.load(std::memory_order_relaxed);
		circular_array<T> *data = M_data.load(std::memory_order_relaxed);
		size_t front = M_front.load(std::memory_order_seq_cst);

		if (front == back || front == back + 1) { return false; }
		M_back.store(--back, std::memory_order_seq_cst);

		bool status = true;

		item = data->pop(back);
		if (front == back) {
			M_back.store(back + 1, std::memory_order_seq_cst);
			if (!M_front.compare_exchange_strong(front, front + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) { status = false; }
		}

		return status;
	}

	bool steal(T &item)
	{
		size_t front = M_front.load();
		size_t back = M_back.load();

		if (back == front || back + 1 == front) { return false; }

		circular_array<T> *data = M_data.load(std::memory_order_consume);
		item = data->pop(front);
		return M_front.compare_exchange_strong(front, front + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
	}

  protected:
	circular_array<T> *resize(circular_array<T> *data, size_t back, size_t front)
	{
		circular_array<T> *new_data = data->resize(back, front);
		delete M_old.load(std::memory_order_relaxed);
		M_old.store(data, std::memory_order_relaxed);
		M_data.store(new_data, std::memory_order_relaxed);
		return new_data;
	}
};

template <unsigned int size>
union chunk_item {
	char M_data[size];
	chunk_item *M_next = nullptr;
};

template <typename T, unsigned int block_size>
class pool_allocator {
	using chunk = chunk_item<sizeof(T)>;
	struct block_ptr {
		chunk *M_ptr = nullptr;
		unsigned int M_tag = 0;
	};
	std::atomic<block_ptr> M_allocator_head;
	std::vector<chunk *> M_allocated_blocks{nullptr};

	std::atomic_bool M_block_allocating{false};

  public:
	pool_allocator()
	{
		block_ptr ptr{nullptr, 0};
		M_allocator_head.store(ptr);
		allocate_block();
	}
	~pool_allocator()
	{
		for (auto &block : M_allocated_blocks) { delete[] block; }
	}

	void allocate_block()
	{
		if (M_block_allocating.exchange(true) == false) {
			chunk *new_block = new chunk[block_size];
			M_allocated_blocks.push_back(new_block);
			for (unsigned int i = 0; i < block_size - 1; ++i) { new_block[i].M_next = &new_block[i + 1]; }
			block_ptr new_head{new_block, 0};
			block_ptr curr_head = M_allocator_head.load();
			do {
				new_block[block_size - 1].M_next = curr_head.M_ptr;
				new_head.M_tag = curr_head.M_tag + 1;
			} while (!M_allocator_head.compare_exchange_strong(curr_head, new_head));

			M_block_allocating.store(false);
		}
	}

	template <typename O = T>
	O *allocate()
	{
		block_ptr head = M_allocator_head.load();
		block_ptr new_head{nullptr, 0};
		do {
			while (head.M_ptr == nullptr) [[unlikely]] {
				allocate_block();
				head = M_allocator_head.load();
			}
			new_head.M_ptr = head.M_ptr->M_next;
			new_head.M_tag = head.M_tag + 1;
		} while (!M_allocator_head.compare_exchange_strong(head, new_head));

		return new (head.M_ptr) O;
	}

	template <typename O = T, typename... Args>
	O *allocate(Args &&...args)
	{
		block_ptr head = M_allocator_head.load();
		block_ptr new_head{nullptr, 0};
		do {
			while (head.M_ptr == nullptr) [[unlikely]] {
				allocate_block();
				head = M_allocator_head.load();
			}
			new_head.M_ptr = head.M_ptr->M_next;
			new_head.M_tag = head.M_tag + 1;
		} while (!M_allocator_head.compare_exchange_strong(head, new_head));

		return new (head.M_ptr) O(args...);
	}

	template <typename O = T>
	void deallocate(O *ptr)
	{
		ptr->~O();
		block_ptr new_head{reinterpret_cast<chunk *>(ptr), 0};
		block_ptr head = M_allocator_head.load();
		do {
			new_head.M_ptr->M_next = head.M_ptr;
			new_head.M_tag = head.M_tag + 1;
		} while (!M_allocator_head.compare_exchange_strong(head, new_head));
	}
};

using task_queue = work_stealing_queue<std::coroutine_handle<>>;

struct thread_status {
	enum class STATUS { NEW, READY, RUNNING, SUSPENDED };
	std::atomic<STATUS> M_status;
};

struct scheduler_task {
	std::coroutine_handle<> M_handle;

	struct promise_type {
		auto initial_suspend() const noexcept
		{
			return std::suspend_always{};
		}
		auto final_suspend() const noexcept
		{
			return std::suspend_never{};
		}

		void return_void() {}

		void unhandled_exception() {}

		scheduler_task get_return_object()
		{
			return scheduler_task(std::coroutine_handle<promise_type>::from_promise(*this));
		}
	};

	scheduler_task(std::coroutine_handle<> handle) : M_handle(handle) {}

	std::coroutine_handle<> handle()
	{
		return M_handle;
	}
};

struct thread_context {
	std::thread M_thread;
	thread_status M_thread_status;
	std::coroutine_handle<> M_waiting_channel;
	task_queue *M_tasks;
};

class scheduler {
	static thread_local unsigned int M_thread_id;
	static thread_local unsigned int M_coro_scheduler_id;
	static unsigned int M_coro_scheduler_count;
	unsigned int M_id = 0;

	std::atomic_uint M_total_threads{0};
	std::atomic_uint M_total_suspended_threads{0};
	std::atomic_uint M_total_ready_threads{0};
	std::atomic_uint M_total_running_threads{0};

	std::vector<thread_context *> M_thread_cxts;

	std::mutex M_task_mutex;

	std::mutex M_spawn_thread_mutex;
	std::mutex M_global_task_queue_mutex;
	std::atomic_flag M_task_wait_flag = ATOMIC_FLAG_INIT;

	bool M_stop_requested = false;

  public:
	scheduler();
	scheduler(unsigned int threa);
	~scheduler();

	void schedule(const std::coroutine_handle<> &handle) noexcept;

	auto get_next_coroutine() noexcept -> std::coroutine_handle<>;

	void spawn_workers(const unsigned int &count);

  protected:
	void init_thread();
	void set_thread_suspended() noexcept;
	void set_thread_ready() noexcept;
	void set_thread_running() noexcept;
	void set_thread_status(thread_status::STATUS status) noexcept;

	scheduler_task awaiter();
	std::coroutine_handle<> get_waiting_channel() noexcept;
	bool peek_next_coroutine(std::coroutine_handle<> &handle) noexcept;
	bool steal_task(std::coroutine_handle<> &handle) noexcept;
};

struct uring_data {
	using allocator = pool_allocator<uring_data, 128>;

	scheduler *M_scheduler = nullptr;
	allocator *M_allocator = nullptr;
	std::coroutine_handle<> M_handle;
	int M_result = 0;
	unsigned int M_flags = 0;
	std::atomic_bool M_handle_ctl{false};
	std::atomic_bool M_destroy_ctl{false};

	void destroy()
	{
		M_allocator->deallocate(this);
	}
};

class uring_awaiter {
	uring_data *M_data;

  public:
	uring_awaiter(const uring_awaiter &) = delete;
	uring_awaiter &operator=(const uring_awaiter &) = delete;

	uring_awaiter(uring_awaiter &&Other)
	{
		M_data = Other.M_data;

		Other.M_data = nullptr;
	}

	uring_awaiter &operator=(uring_awaiter &&Other)
	{
		M_data = Other.M_data;

		Other.M_data = nullptr;

		return *this;
	}

	auto operator co_await()
	{
		struct {
			uring_data *M_data = nullptr;
			bool await_ready() const noexcept
			{
				return M_data->M_handle_ctl.load(std::memory_order_relaxed);
			}

			auto await_suspend(const std::coroutine_handle<> &handle) noexcept -> std::coroutine_handle<>
			{
				M_data->M_handle = handle;
				auto schd = M_data->M_scheduler;
				if (M_data->M_handle_ctl.exchange(true, std::memory_order_acq_rel)) { return handle; }
				return schd->get_next_coroutine();
			}
			auto await_resume() const noexcept
			{
				// Acquire the changes to the result
				M_data->M_handle_ctl.load();
				struct {
					int result;
					unsigned int flags;
					operator int()
					{
						return result;
					}
				} result{M_data->M_result, M_data->M_flags};
				return result;
			}
		} awaiter{M_data};

		return awaiter;
	}

	uring_awaiter(uring_data::allocator *allocator)
	{
		M_data = allocator->allocate();
		M_data->M_allocator = allocator;
	}

	~uring_awaiter()
	{
		if (M_data != nullptr && M_data->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { M_data->destroy(); }
	}

	uring_data *get_data() const noexcept
	{
		return M_data;
	}

	void via(scheduler *s)
	{
		this->M_data->M_scheduler = s;
	}
};

template <typename T>
concept IO_URING_OP = requires(T a, io_uring *const uring, uring_data::allocator *alloc) {
	{ a.run(uring) } -> std::same_as<bool>;
	{ a.get_future(alloc) } -> std::same_as<uring_awaiter>;
};

struct io_uring_future {
	uring_data *M_data;

	uring_awaiter get_future(uring_data::allocator *allocator)
	{
		uring_awaiter awaiter(allocator);
		M_data = awaiter.get_data();
		return awaiter;
	}
};

struct io_uring_op_nop_t : public io_uring_future {
	unsigned char M_sqe_flags;

	io_uring_op_nop_t() = default;

	io_uring_op_nop_t(unsigned char &sqe_flags) : M_sqe_flags{sqe_flags} {}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_nop(sqe);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_poll_add_t : public io_uring_future {
	int M_fd;
	unsigned M_poll_mask;
	unsigned char M_sqe_flags;

	io_uring_op_poll_add_t() = default;

	io_uring_op_poll_add_t(const int &fd, const unsigned &poll_mask, unsigned char &sqe_flags) : M_fd{fd}, M_poll_mask{poll_mask}, M_sqe_flags{sqe_flags} {}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_poll_add(sqe, M_fd, M_poll_mask);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_cancel_t : public io_uring_future {
	void *M_user_data;
	int M_flags;
	unsigned char M_sqe_flags;

	io_uring_op_cancel_t() = default;

	io_uring_op_cancel_t(void *const user_data, const int &flags, unsigned char &sqe_flags) : M_user_data{user_data}, M_flags{flags}, M_sqe_flags{sqe_flags} {}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_cancel(sqe, M_user_data, M_flags);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_openat_t : public io_uring_future {
	int M_dir;
	const char *M_filename;
	int M_flags;
	mode_t M_mode;
	unsigned char M_sqe_flags;

	io_uring_op_openat_t() = default;

	io_uring_op_openat_t(const int &dir, const char *const &filename, const int &flags, const mode_t &mode, unsigned char &sqe_flags)
		: M_dir{dir}, M_filename{filename}, M_flags{flags}, M_mode{mode}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_openat(sqe, M_dir, M_filename, M_flags, M_mode);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_read_t : public io_uring_future {
	int M_fd;
	void *M_buffer;
	unsigned M_bytes;
	off_t M_offset;
	unsigned char M_sqe_flags;

	io_uring_op_read_t() = default;

	io_uring_op_read_t(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, unsigned char &sqe_flags)
		: M_fd{fd}, M_buffer{buffer}, M_bytes{bytes}, M_offset{offset}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_read(sqe, M_fd, M_buffer, M_bytes, M_offset);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_readv_t : public io_uring_future {
	int M_fd;
	iovec *M_iovecs;
	unsigned int M_count;
	off_t M_offset;
	unsigned char M_sqe_flags;

	io_uring_op_readv_t() = default;

	io_uring_op_readv_t(const int &fd, iovec *const &iovecs, const unsigned int &count, const off_t &offset, unsigned char &sqe_flags)
		: M_fd{fd}, M_iovecs{iovecs}, M_count{count}, M_offset{offset}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_readv(sqe, M_fd, M_iovecs, M_count, M_offset);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_read_provide_buffer_t : public io_uring_future {
	int M_fd;
	int M_gbid;
	unsigned M_bytes;
	off_t M_offset;
	unsigned char M_sqe_flags;

	io_uring_op_read_provide_buffer_t() = default;

	io_uring_op_read_provide_buffer_t(const int &fd, const int &gbid, const unsigned &bytes, const off_t &offset, unsigned char &sqe_flags)
		: M_fd{fd}, M_gbid{gbid}, M_bytes{bytes}, M_offset{offset}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_read(sqe, M_fd, nullptr, M_bytes, M_offset);
		sqe->flags |= (M_sqe_flags | IOSQE_BUFFER_SELECT);
		sqe->buf_group = M_gbid;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_read_fixed_t : public io_uring_future {
	int M_fd;
	void *M_buffer;
	unsigned M_bytes;
	off_t M_offset;
	int M_buf_index;
	unsigned char M_sqe_flags;

	io_uring_op_read_fixed_t() = default;

	io_uring_op_read_fixed_t(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, const int &buf_index, unsigned char &sqe_flags)
		: M_fd{fd}, M_buffer{buffer}, M_bytes{bytes}, M_offset{offset}, M_buf_index{buf_index}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_read_fixed(sqe, M_fd, M_buffer, M_bytes, M_offset, M_buf_index);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_write_t : public io_uring_future {
	int M_fd;
	void *M_buffer;
	unsigned M_bytes;
	off_t M_offset;
	unsigned char M_sqe_flags;

	io_uring_op_write_t() = default;

	io_uring_op_write_t(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, unsigned char &sqe_flags)
		: M_fd{fd}, M_buffer{buffer}, M_bytes{bytes}, M_offset{offset}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_write(sqe, M_fd, M_buffer, M_bytes, M_offset);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_writev_t : public io_uring_future {
	int M_fd;
	iovec *M_iovecs;
	unsigned int M_count;
	off_t M_offset;
	unsigned char M_sqe_flags;

	io_uring_op_writev_t() = default;

	io_uring_op_writev_t(const int &fd, iovec *const &iovecs, const unsigned int &count, const off_t &offset, unsigned char &sqe_flags)
		: M_fd{fd}, M_iovecs{iovecs}, M_count{count}, M_offset{offset}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_writev(sqe, M_fd, M_iovecs, M_count, M_offset);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_write_fixed_t : public io_uring_future {
	int M_fd;
	void *M_buffer;
	unsigned M_bytes;
	off_t M_offset;
	int M_buf_index;
	unsigned char M_sqe_flags;

	io_uring_op_write_fixed_t() = default;

	io_uring_op_write_fixed_t(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, const int &buf_index, unsigned char &sqe_flags)
		: M_fd{fd}, M_buffer{buffer}, M_bytes{bytes}, M_offset{offset}, M_buf_index{buf_index}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_write_fixed(sqe, M_fd, M_buffer, M_bytes, M_offset, M_buf_index);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_timeout_t : public io_uring_future {
	__kernel_timespec *M_time;
	unsigned char M_sqe_flags;

	io_uring_op_timeout_t() = default;

	io_uring_op_timeout_t(__kernel_timespec *const &t, unsigned char &sqe_flags) : M_time{t}, M_sqe_flags{sqe_flags} {}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_timeout(sqe, M_time, 0, 0);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_link_timeout_t : public io_uring_future {
	__kernel_timespec *M_time;
	unsigned M_flags;
	unsigned char M_sqe_flags;

	io_uring_op_link_timeout_t() = default;

	io_uring_op_link_timeout_t(__kernel_timespec *const &t, const unsigned flags, unsigned char &sqe_flags) : M_time{t}, M_flags{flags}, M_sqe_flags{sqe_flags} {}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_link_timeout(sqe, M_time, M_flags);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_recv_t : public io_uring_future {
	int M_fd;
	void *M_buffer;
	size_t M_length;
	int M_flags;
	unsigned char M_sqe_flags;

	io_uring_op_recv_t() = default;

	io_uring_op_recv_t(const int &fd, void *const &buffer, const size_t &length, const int &flags, unsigned char &sqe_flags)
		: M_fd{fd}, M_buffer{buffer}, M_length{length}, M_flags{flags}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_recv(sqe, M_fd, M_buffer, M_length, M_flags);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_recv_provide_buffer_t : public io_uring_future {
	int M_fd;
	int M_gbid;
	size_t M_length;
	int M_flags;
	unsigned char M_sqe_flags;

	io_uring_op_recv_provide_buffer_t() = default;

	io_uring_op_recv_provide_buffer_t(const int &fd, const int &gbid, const size_t &length, const int &flags, unsigned char &sqe_flags)
		: M_fd{fd}, M_gbid{gbid}, M_length{length}, M_flags{flags}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_recv(sqe, M_fd, nullptr, M_length, M_flags);
		io_uring_sqe_set_flags(sqe, M_sqe_flags | IOSQE_BUFFER_SELECT);
		sqe->buf_group = M_gbid;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_accept_t : public io_uring_future {
	int M_fd;
	sockaddr *M_client_info;
	socklen_t *M_socklen;
	int M_flags;
	unsigned char M_sqe_flags;

	io_uring_op_accept_t() = default;

	io_uring_op_accept_t(const int &fd, sockaddr *const &client_info, socklen_t *const &socklen, const int &flags, unsigned char &sqe_flags)
		: M_fd{fd}, M_client_info{client_info}, M_socklen{socklen}, M_flags{flags}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_accept(sqe, M_fd, M_client_info, M_socklen, 0);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_send_t : public io_uring_future {
	int M_fd;
	void *M_buffer;
	size_t M_length;
	int M_flags;
	unsigned char M_sqe_flags;

	io_uring_op_send_t() = default;

	io_uring_op_send_t(const int &fd, void *const &buffer, const size_t &length, const int &flags, unsigned char &sqe_flags)
		: M_fd{fd}, M_buffer{buffer}, M_length{length}, M_flags{flags}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_send(sqe, M_fd, M_buffer, M_length, M_flags);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_close_t : public io_uring_future {
	int M_fd;
	unsigned char M_sqe_flags;

	io_uring_op_close_t() = default;

	io_uring_op_close_t(const int &fd, unsigned char &sqe_flags) : M_fd{fd}, M_sqe_flags{sqe_flags} {}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_close(sqe, M_fd);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_statx_t : public io_uring_future {
	int M_dfd;
	const char *M_path;
	int M_flags;
	unsigned M_mask;
	struct statx *M_statxbuf;
	unsigned char M_sqe_flags;

	io_uring_op_statx_t() = default;

	io_uring_op_statx_t(int dfd, const char *path, int flags, unsigned mask, struct statx *statxbuf, unsigned char &sqe_flags)
		: M_dfd{dfd}, M_path{path}, M_flags{flags}, M_mask{mask}, M_statxbuf{statxbuf}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_statx(sqe, M_dfd, M_path, M_flags, M_mask, M_statxbuf);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

struct io_uring_op_provide_buffer_t : public io_uring_future {
	void *M_addr;
	int M_buffer_length;
	int M_buffer_count;
	int M_bgid;
	int M_bid;
	unsigned char M_sqe_flags;

	io_uring_op_provide_buffer_t() = default;

	io_uring_op_provide_buffer_t(void *const addr, int buffer_length, int buffer_count, int bgid, int bid, unsigned char &sqe_flags)
		: M_addr{addr}, M_buffer_length{buffer_length}, M_buffer_count{buffer_count}, M_bgid{bgid}, M_bid{bid}, M_sqe_flags{sqe_flags}
	{
	}

	bool run(io_uring *const uring)
	{
		io_uring_sqe *sqe;
		if ((sqe = io_uring_get_sqe(uring)) == nullptr) { return false; }
		io_uring_prep_provide_buffers(sqe, M_addr, M_buffer_length, M_buffer_count, M_bgid, M_bid);
		sqe->flags |= M_sqe_flags;
		io_uring_sqe_set_data(sqe, M_data);
		return true;
	}
};

using io_uring_op = std::variant<io_uring_op_timeout_t, io_uring_op_openat_t, io_uring_op_read_t, io_uring_op_close_t, io_uring_op_cancel_t, io_uring_op_statx_t,
								 io_uring_op_write_t, io_uring_op_recv_t, io_uring_op_accept_t, io_uring_op_read_provide_buffer_t, io_uring_op_write_fixed_t, io_uring_op_writev_t,
								 io_uring_op_nop_t, io_uring_op_send_t, io_uring_op_recv_provide_buffer_t, io_uring_op_poll_add_t, io_uring_op_provide_buffer_t,
								 io_uring_op_read_fixed_t, io_uring_op_readv_t, io_uring_op_link_timeout_t>;

template <typename IO_SERVICE>
class io_operation {
	IO_SERVICE *M_io_service;

  public:
	explicit io_operation(IO_SERVICE *service) : M_io_service{service} {}

	auto nop(unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_nop_t(sqe_flags));
	}

	auto delay(const unsigned long &sec, const unsigned long &nsec, unsigned char sqe_flags = 0)
	{
		int tfd = timerfd_create(CLOCK_REALTIME, 0);
		itimerspec spec;
		spec.it_value.tv_sec = sec;
		spec.it_value.tv_nsec = nsec;
		spec.it_interval.tv_sec = 0;
		spec.it_interval.tv_nsec = 0;
		timerfd_settime(tfd, 0, &spec, NULL);

		poll_add(tfd, POLL_IN, sqe_flags | IOSQE_IO_HARDLINK);
		return close(tfd, sqe_flags);
	}

	auto poll_add(const int &fd, const unsigned &poll_mask, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_poll_add_t(fd, poll_mask, sqe_flags));
	}

	auto cancel(uring_awaiter &awaiter, const int &flags, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_cancel_t(awaiter.get_data(), flags, sqe_flags));
	}

	auto openat(const int &dfd, const char *const &filename, const int &flags, const mode_t &mode, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_openat_t(dfd, filename, flags, mode, sqe_flags));
	}

	auto read(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_read_t(fd, buffer, bytes, offset, sqe_flags));
	}

	auto read(const int &fd, const int &gbid, const unsigned &bytes, const off_t &offset, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_read_provide_buffer_t(fd, gbid, bytes, offset, sqe_flags));
	}

	auto readv(const int &fd, iovec *const &iovecs, const unsigned int &count, const off_t &offset, unsigned char &sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_readv_t(fd, iovecs, count, offset, sqe_flags));
	}

	auto read_fixed(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, const int &buf_index, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_read_fixed_t(fd, buffer, bytes, offset, buf_index, sqe_flags));
	}

	auto write(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_write_t(fd, buffer, bytes, offset, sqe_flags));
	}

	auto writev(const int &fd, iovec *const &iovecs, const unsigned int &count, const off_t &offset, unsigned char &sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_writev_t(fd, iovecs, count, offset, sqe_flags));
	}

	auto write_fixed(const int &fd, void *const &buffer, const unsigned &bytes, const off_t &offset, const int &buf_index, unsigned char sqe_flags = 0)
	{
		return M_io_service->submit_io(io_uring_op_write_fixed_t(fd, buffer, bytes, offset, buf_index, sqe_flags));
	}

	auto recv(const int &fd, void *const &buffer, const size_t &length, const int &flags, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_recv_t(fd, buffer, length, flags, sqe_flags));
	}

	auto recv(const int &fd, const int &gbid, const size_t &length, const int &flags, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_recv_provide_buffer_t(fd, gbid, length, flags, sqe_flags));
	}

	auto accept(const int &fd, sockaddr *const &client_info, socklen_t *const &socklen, const int &flags, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_accept_t(fd, client_info, socklen, flags, sqe_flags));
	}

	auto send(const int &fd, void *const &buffer, const size_t &length, const int &flags, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_send_t(fd, buffer, length, flags, sqe_flags));
	}

	auto close(const int &fd, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_close_t(fd, sqe_flags));
	}

	auto statx(int dfd, const char *path, int flags, unsigned mask, struct statx *statxbuf, unsigned char sqe_flags = 0)
	{
		return M_io_service->submit_io(io_uring_op_statx_t(dfd, path, flags, mask, statxbuf, sqe_flags));
	}

	auto timeout(__kernel_timespec *const &t, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_timeout_t(t, sqe_flags));
	}

	auto link_timeout(__kernel_timespec *const &t, const unsigned &flags, unsigned char sqe_flags = 0) -> uring_awaiter
	{
		return M_io_service->submit_io(io_uring_op_link_timeout_t(t, flags, sqe_flags));
	}

	auto provide_buffer(void *const addr, int buffer_length, int buffer_count, int bgid, int bid = 0, unsigned char sqe_flags = 0)
	{
		return M_io_service->submit_io(io_uring_op_provide_buffer_t(addr, buffer_length, buffer_count, bgid, bid, sqe_flags));
	}
};

enum class IO_OP_TYPE { BATCH, LINK };

template <typename IO_Service, IO_OP_TYPE Type>
class io_operation_detached : public io_operation<io_operation_detached<IO_Service, Type>> {
	IO_Service *M_io_service;
	std::vector<io_uring_op> M_io_operations;

  public:
	explicit io_operation_detached(IO_Service *io_service) : io_operation<io_operation_detached<IO_Service, Type>>(this), M_io_service{io_service} {}

	std::vector<io_uring_op> &operations()
	{
		return M_io_operations;
	}

	template <IO_URING_OP OP>
	auto submit_io(OP &&operation) -> uring_awaiter
	{
		auto future = operation.get_future(M_io_service->get_awaiter_allocator());
		M_io_operations.push_back(std::forward<OP>(operation));
		return future;
	}
};

template <typename IO_Service>
using io_batch = io_operation_detached<IO_Service, IO_OP_TYPE::BATCH>;

template <typename IO_Service>
using io_link = io_operation_detached<IO_Service, IO_OP_TYPE::LINK>;

class io_op_pipeline {
	io_work_queue<io_uring_op> M_io_work_queue;
	io_uring_op M_overflow_work;
	bool M_has_overflow_work = false;

  public:
	io_op_pipeline(size_t capacity) : M_io_work_queue(capacity) {}

	template <typename T>
	void enqueue(T &&val)
	{
		M_io_work_queue.enqueue(io_uring_op(std::forward<T>(val)));
	}

	void enqueue(std::vector<io_uring_op> &items)
	{
		M_io_work_queue.bulk_enqueue(items);
	}

	int init_io_uring_ops(io_uring *const uring)
	{
		auto submit_operation = [&uring](IO_URING_OP auto &&item) { return item.run(uring); };

		unsigned int completed = 0;
		if (M_has_overflow_work) {
			if (!std::visit(submit_operation, M_overflow_work)) {
				return -1;
			} else {
				M_has_overflow_work = false;
				++completed;
			}
		}

		io_uring_op op;
		while (M_io_work_queue.dequeue(op)) {
			if (!std::visit(submit_operation, op)) {
				return -1;
				M_overflow_work = op;
				M_has_overflow_work = true;
			} else {
				++completed;
			}
		}
		return completed;
	}

	bool empty() const noexcept
	{
		return M_io_work_queue.empty();
	}
};

class io_service : public io_operation<io_service> {
	static thread_local unsigned int M_thread_id;
	static thread_local uring_data::allocator *M_uio_data_allocator;
	static thread_local io_op_pipeline *M_io_queue;

	std::vector<io_op_pipeline *> M_io_queues;
	std::vector<uring_data::allocator *> M_uio_data_allocators;

	std::atomic<io_op_pipeline *> M_io_queue_overflow{nullptr};

	io_uring M_uring;

	unsigned int M_entries;
	unsigned int M_flags;

	std::thread M_io_cq_thread;
	std::atomic_bool M_io_sq_running{false};
	std::atomic_int M_threads{0};

	std::atomic_bool M_stop_requested{false};

  public:
	io_service(const u_int &entries, const u_int &flags);
	io_service(const u_int &entries, io_uring_params &params);

	~io_service();

	template <size_t n>
	bool register_buffer(iovec (&io_vec)[n])
	{
		return io_uring_register_buffers(&M_uring, io_vec, n) == 0 ? true : false;
	}

	auto batch()
	{
		return io_batch<io_service>(this);
	}

	auto link()
	{
		return io_link<io_service>(this);
	}

	uring_data::allocator *get_awaiter_allocator()
	{
		setup_thread_context();
		return M_uio_data_allocator;
	}

	void submit(io_batch<io_service> &batch);

	void submit(io_link<io_service> &link);

	template <IO_URING_OP OP>
	auto submit_io(OP &&operation) -> uring_awaiter
	{
		setup_thread_context();

		auto future = operation.get_future(M_uio_data_allocator);

		M_io_queue->enqueue(std::forward<OP>(operation));

		if (!(operation.M_sqe_flags & (IOSQE_IO_HARDLINK | IOSQE_IO_LINK))) { submit(); }

		return future;
	}

	unsigned int get_buffer_index(unsigned int &flag)
	{
		return flag >> 16;
	}

  protected:
	void submit();

	void io_loop() noexcept;

	bool io_queue_empty() const noexcept;

	void setup_thread_context();

	void handle_completion(io_uring_cqe *cqe);
};

template <typename T>
concept Resume_VIA = requires(T a, scheduler *s) {
	{ a.via(s) };
};

struct get_scheduler {
	scheduler *M_scheduler;
	constexpr bool await_ready() const noexcept
	{
		return true;
	}
	constexpr bool await_suspend(const std::coroutine_handle<> &) const noexcept
	{
		return false;
	}
	scheduler *await_resume() const noexcept
	{
		return M_scheduler;
	}
};

struct get_stop_token {
	std::stop_source *M_stop_source;
	constexpr bool await_ready() const noexcept
	{
		return true;
	}
	constexpr bool await_suspend(const std::coroutine_handle<> &) const noexcept
	{
		return false;
	}
	std::stop_token await_resume() const noexcept
	{
		return M_stop_source->get_token();
	}
};

template <typename Promise>
struct cancel_awaiter {
	Promise *M_promise;
	bool await_ready() const noexcept
	{
		return M_promise->M_cancel_handle_ctl.load(std::memory_order_relaxed);
	}

	auto await_suspend(const std::coroutine_handle<> &handle) noexcept
	{
		M_promise->M_cancel_continuation = handle;

		return M_promise->M_cancel_handle_ctl.exchange(true, std::memory_order_acq_rel) ? handle : M_promise->M_scheduler->get_next_coroutine();
	}

	void await_resume() const noexcept {}
};

struct Awaiter_Transforms {
	scheduler *M_scheduler{nullptr};
	scheduler *M_cancel_scheduler{nullptr};
	std::coroutine_handle<> M_cancel_continuation;
	std::stop_source M_stop_source;
	std::atomic_bool M_cancel_handle_ctl{false};
	template <Resume_VIA A>
	A &await_transform(A &awaiter)
	{
		awaiter.via(M_scheduler);
		return awaiter;
	}

	template <Resume_VIA A>
	A &&await_transform(A &&awaiter)
	{
		awaiter.via(M_scheduler);
		return std::move(awaiter);
	}

	get_scheduler &await_transform(get_scheduler &gs)
	{
		gs.M_scheduler = M_scheduler;
		return gs;
	}

	get_scheduler &&await_transform(get_scheduler &&gs)
	{
		gs.M_scheduler = M_scheduler;
		return std::move(gs);
	}

	get_stop_token &await_transform(get_stop_token &st)
	{
		st.M_stop_source = &M_stop_source;
		return st;
	}

	get_stop_token &&await_transform(get_stop_token &&st)
	{
		st.M_stop_source = &M_stop_source;
		return std::move(st);
	}

	template <typename T>
	cancel_awaiter<T> &await_transform(cancel_awaiter<T> &ca)
	{
		ca.M_promise->M_cancel_scheduler = M_scheduler;
		return ca;
	}

	template <typename T>
	cancel_awaiter<T> &&await_transform(cancel_awaiter<T> &&ca)
	{
		ca.M_promise->M_cancel_scheduler = M_scheduler;
		return std::move(ca);
	}

	template <typename Default>
	Default &&await_transform(Default &&d)
	{
		return static_cast<Default &&>(d);
	}
};

template <typename Promise, typename Return = void>
struct async_awaiter {
	Promise *M_promise;
	bool await_ready() const noexcept
	{
		return M_promise->M_handle_ctl.load(std::memory_order_acquire);
	}

	auto await_suspend(const std::coroutine_handle<> &handle) noexcept -> std::coroutine_handle<>
	{
		M_promise->M_continuation = handle;
		if (M_promise->M_handle_ctl.exchange(true, std::memory_order_acq_rel)) { return handle; }
		return M_promise->M_scheduler->get_next_coroutine();
	}

	auto await_resume() const noexcept -> Return &
	{
		return M_promise->M_value;
	}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<Promise>(M_promise);
	}

	void via(scheduler *s)
	{
		this->M_promise->M_continuation_scheduler = s;
	}
};

template <typename Promise>
struct async_awaiter<Promise, void> {
	Promise *M_promise;
	bool await_ready() const noexcept
	{
		return M_promise->M_handle_ctl.load(std::memory_order_acquire);
	}

	auto await_suspend(const std::coroutine_handle<> &handle) const noexcept -> std::coroutine_handle<>
	{
		M_promise->M_continuation = handle;
		if (M_promise->M_handle_ctl.exchange(true, std::memory_order_acq_rel)) { return handle; }
		return M_promise->M_scheduler->get_next_coroutine();
	}

	void await_resume() const noexcept {}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<Promise>(M_promise);
	}

	void via(scheduler *s)
	{
		this->M_promise->M_continuation_scheduler = s;
	}
};

template <typename Promise>
struct async_final_suspend {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	auto await_suspend(const std::coroutine_handle<> &handle) const noexcept -> std::coroutine_handle<>
	{
		if (M_promise->M_cancel_handle_ctl.exchange(true, std::memory_order_acquire)) { M_promise->M_cancel_scheduler->schedule(M_promise->M_cancel_continuation); }

		std::coroutine_handle<> continuation;
		if (M_promise->M_handle_ctl.exchange(true, std::memory_order_acq_rel)) {
			if (M_promise->M_continuation_scheduler == M_promise->M_scheduler) {
				continuation = M_promise->M_continuation;
			} else {
				M_promise->M_continuation_scheduler->schedule(M_promise->M_continuation);
				continuation = M_promise->M_scheduler->get_next_coroutine();
			}
		} else {
			continuation = M_promise->M_scheduler->get_next_coroutine();
		}

		if (M_promise->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { handle.destroy(); }

		return continuation;
	}

	constexpr void await_resume() const noexcept {}
};

template <typename Ret = void>
struct async {
	using Return = std::remove_reference<Ret>::type;
	struct promise_type : public Awaiter_Transforms {
		Return M_value;
		scheduler *M_continuation_scheduler{nullptr};
		std::coroutine_handle<> M_continuation;
		std::atomic_bool M_handle_ctl{false};
		std::atomic_bool M_destroy_ctl{false};

		std::suspend_always initial_suspend() const noexcept
		{
			return {};
		}

		auto final_suspend() noexcept
		{
			return async_final_suspend{this};
		}

		void return_value(const Return &value) noexcept
		{
			M_value = std::move(value);
		}

		void unhandled_exception() {}

		async get_return_object()
		{
			return async(this);
		}
	};

	promise_type *M_promise;
	async(promise_type *promise) : M_promise(promise) {}

	async(const async &) = delete;
	async operator=(const async &) = delete;

	async(async &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
	}
	async &operator=(async &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	~async()
	{
		if (M_promise != nullptr && M_promise->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { std::coroutine_handle<promise_type>::from_promise(*M_promise).destroy(); }
	}

	auto operator co_await()
	{
		return async_awaiter<promise_type, Return>{M_promise};
	}

	auto schedule_on(scheduler *s)
	{
		if (this->M_promise->M_scheduler == nullptr) {
			this->M_promise->M_scheduler = s;
			s->schedule(std::coroutine_handle<promise_type>::from_promise(*M_promise));
		}
		return async_awaiter<promise_type, Return>{M_promise};
	}

	void via(scheduler *s)
	{
		this->M_promise->M_continuation_scheduler = s;
		if (this->M_promise->M_scheduler == nullptr) {
			this->M_promise->M_scheduler = s;
			s->schedule(std::coroutine_handle<promise_type>::from_promise(*M_promise));
		}
	}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<promise_type>(M_promise);
	}
};

template <>
struct async<void> {
	using Return = void;
	struct promise_type : public Awaiter_Transforms {
		std::coroutine_handle<> M_continuation;
		scheduler *M_continuation_scheduler{nullptr};
		std::atomic_bool M_handle_ctl{false};
		std::atomic_bool M_destroy_ctl{false};

		std::suspend_always initial_suspend() const noexcept
		{
			return {};
		}

		auto final_suspend() noexcept
		{
			return async_final_suspend{this};
		}

		void return_void() const noexcept {}

		void unhandled_exception() {}

		async get_return_object()
		{
			return async(this);
		}
	};

	promise_type *M_promise;
	async(promise_type *promise) : M_promise(promise) {}

	async(const async &) = delete;
	async &operator=(const async &) = delete;

	async(async &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
	}
	async &operator=(async &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	~async()
	{
		if (M_promise != nullptr && M_promise->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { std::coroutine_handle<promise_type>::from_promise(*M_promise).destroy(); }
	}

	auto operator co_await()
	{
		return async_awaiter<promise_type, void>{M_promise};
	}

	auto schedule_on(scheduler *s)
	{
		if (this->M_promise->M_scheduler == nullptr) {
			this->M_promise->M_scheduler = s;
			s->schedule(std::coroutine_handle<promise_type>::from_promise(*M_promise));
		}
		return async_awaiter<promise_type, void>{M_promise};
	}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<promise_type>(M_promise);
	}

	void via(scheduler *s)
	{
		this->M_promise->M_continuation_scheduler = s;
		if (this->M_promise->M_scheduler == nullptr) {
			this->M_promise->M_scheduler = s;
			s->schedule(std::coroutine_handle<promise_type>::from_promise(*M_promise));
		}
	}
};

class event {
	std::coroutine_handle<> M_handle;
	scheduler *M_scheduler = nullptr;
	std::atomic_uint64_t M_event_count = 0;
	std::atomic_bool M_handle_ctl = false;

	struct event_awaiter {
		event *M_promise;

		bool await_ready() const noexcept
		{
			return M_promise->M_event_count.load(std::memory_order_relaxed);
		}

		auto await_suspend(const std::coroutine_handle<> &handle) noexcept
		{
			M_promise->M_handle = handle;
			M_promise->M_handle_ctl.store(true, std::memory_order_relaxed);
			if (M_promise->M_event_count.load(std::memory_order_relaxed)) {
				if (M_promise->M_handle_ctl.exchange(false, std::memory_order_relaxed)) { return handle; }
			}
			return M_promise->M_scheduler->get_next_coroutine();
		}

		auto await_resume() noexcept
		{
			return M_promise->M_event_count.exchange(0, std::memory_order_relaxed);
		}
	};

  public:
	auto operator co_await()
	{
		return event_awaiter{this};
	}

	auto set(uint64_t events_count)
	{
		M_event_count.fetch_add(events_count, std::memory_order_relaxed);
		bool expected = true;
		if (M_handle_ctl.compare_exchange_strong(expected, false, std::memory_order_acquire)) {
			M_scheduler->schedule(M_handle);
			return;
		}
	}

	void via(scheduler *s)
	{
		this->M_scheduler = s;
	}
};

template <typename Promise>
struct generator_promise {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	std::coroutine_handle<> await_suspend(const std::coroutine_handle<> &) const noexcept
	{
		return M_promise->M_continuation;
	}

	constexpr void await_resume() const noexcept {}
};

template <typename Promise>
struct generator_final_suspend {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	std::coroutine_handle<> await_suspend(const std::coroutine_handle<> &) const noexcept
	{
		if (M_promise->M_cancel_handle_ctl.exchange(true, std::memory_order_relaxed)) { M_promise->M_cancel_scheduler->schedule(M_promise->M_cancel_continuation); }
		return M_promise->M_continuation;
	}

	constexpr void await_resume() const noexcept {}
};

template <typename Promise, typename Return>
struct generator_future {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	auto await_suspend(const std::coroutine_handle<> &handle) const noexcept -> std::coroutine_handle<>
	{
		M_promise->M_continuation = handle;
		return std::coroutine_handle<Promise>::from_promise(*M_promise);
	}

	Return await_resume() const noexcept
	{
		return std::move(M_promise->M_value);
	}

	auto cancel()
	{
		this->M_promise->M_stop_source.request_stop();
		return *this;
	}
};

template <typename Ret>
struct generator {
	using Return = std::remove_reference_t<Ret>;
	struct promise_type : public Awaiter_Transforms {
		std::coroutine_handle<> M_continuation;
		Return M_value;

		std::suspend_always initial_suspend() const noexcept
		{
			return {};
		}

		auto final_suspend() noexcept
		{
			return generator_final_suspend<promise_type>{this};
		}

		auto yield_value(const Return &value)
		{
			M_value = value;
			return generator_promise<promise_type>{this};
		}

		void return_value(const Return &value) noexcept
		{
			M_value = std::move(value);
		}

		void unhandled_exception() noexcept {}

		generator<Ret> get_return_object() noexcept
		{
			return generator<Ret>(this);
		}
	};

	promise_type *M_promise;
	generator(promise_type *promise) : M_promise(promise) {}

	generator(const generator &) = delete;
	generator &operator=(const generator &) = delete;

	generator(generator &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
	}
	generator &operator=(generator &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	auto operator co_await()
	{
		return generator_future<promise_type, Ret>{M_promise};
	}

	void via(scheduler *s)
	{
		this->M_promise->M_scheduler = s;
	}

	auto cancel()
	{
		this->M_promise->M_stop_source.request_stop();
		return generator_future<promise_type, Ret>{M_promise};
	}

	operator bool()
	{
		return !std::coroutine_handle<promise_type>::from_promise(*M_promise).done();
	}
};

struct launch_final_awaiter {
	std::atomic_bool &M_destroy_ctl;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	void await_suspend(std::coroutine_handle<> handle) noexcept
	{
		if (M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { handle.destroy(); }
	}
	constexpr void await_resume() const noexcept {}
};

template <typename Return = void>
struct launch {
	struct promise_type : public Awaiter_Transforms {
		std::promise<Return> M_result;
		std::atomic_bool M_destroy_ctl;

		std::suspend_always initial_suspend() noexcept
		{
			return {};
		}
		launch_final_awaiter final_suspend() noexcept
		{
			return {M_destroy_ctl};
		}

		void return_value(const Return &value)
		{
			M_result.set_value(std::move(value));
		}

		void unhandled_exception() {}

		launch<Return> get_return_object()
		{
			return {this};
		}
	};

	launch(promise_type *promise) : M_promise(promise) {}

	launch(const launch &) = delete;
	launch &operator=(const launch &) = delete;

	launch(launch &&l)
	{
		this->M_promise = l.M_promise;
		l.M_promise = nullptr;
	}
	launch &operator=(launch &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	~launch()
	{
		if (M_promise != nullptr && M_promise->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { std::coroutine_handle<promise_type>::from_promise(*M_promise).destroy(); }
	}

	operator Return() const noexcept
	{
		return M_promise->M_result.get_future().get();
	}

	launch<Return> &&schedule_on(scheduler *schd)
	{
		M_promise->M_scheduler = schd;
		schd->schedule(std::coroutine_handle<promise_type>::from_promise(*M_promise));
		return std::move(*this);
	}

	auto cancel()
	{
		this->M_promise->M_stop_source.request_stop();
	}

  private:
	promise_type *M_promise;
};

template <>
struct launch<void> {
	struct promise_type : public Awaiter_Transforms {
		std::promise<int> M_result;
		std::atomic_bool M_destroy_ctl;

		std::suspend_always initial_suspend() noexcept
		{
			return {};
		}
		launch_final_awaiter final_suspend() noexcept
		{
			return {M_destroy_ctl};
		}

		void return_void()
		{
			M_result.set_value(0);
		}

		void unhandled_exception() {}

		launch get_return_object()
		{
			return launch(this);
		}
	};

	launch(promise_type *promise) : M_promise{promise} {}

	launch(const launch &) = delete;
	launch &operator=(const launch &) = delete;

	launch(launch &&l)
	{
		this->M_promise = l.M_promise;
		l.M_promise = nullptr;
	}
	launch &operator=(launch &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	~launch()
	{
		if (M_promise != nullptr && M_promise->M_destroy_ctl.exchange(true, std::memory_order_relaxed)) { std::coroutine_handle<promise_type>::from_promise(*M_promise).destroy(); }
	}

	void join()
	{
		M_promise->M_result.get_future().get();
	}

	launch<void> &&schedule_on(scheduler *schd)
	{
		M_promise->M_scheduler = schd;
		schd->schedule(std::coroutine_handle<promise_type>::from_promise(*M_promise));
		return std::move(*this);
	}

	auto cancel()
	{
		this->M_promise->M_stop_source.request_stop();
	}

  private:
	promise_type *M_promise;
};

template <typename Promise, typename Return = void>
struct task_awaiter {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	auto await_suspend(const std::coroutine_handle<> &handle) const noexcept -> std::coroutine_handle<>
	{
		M_promise->M_continuation = handle;
		return std::coroutine_handle<Promise>::from_promise(*M_promise);
	}

	constexpr Return await_resume() const noexcept
	{
		return M_promise->M_value;
	}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<Promise>(M_promise);
	}
};

template <typename Promise>
struct task_awaiter<Promise, void> {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	auto await_suspend(const std::coroutine_handle<> &handle) const noexcept -> std::coroutine_handle<>
	{
		M_promise->M_continuation = handle;
		return std::coroutine_handle<Promise>::from_promise(*M_promise);
	}

	constexpr void await_resume() const noexcept {}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<Promise>(M_promise);
	}
};

template <typename Promise>
struct task_final_awaiter {
	Promise *M_promise;
	constexpr bool await_ready() const noexcept
	{
		return false;
	}

	std::coroutine_handle<> await_suspend(const std::coroutine_handle<> &) noexcept
	{
		if (M_promise->M_cancel_handle_ctl.exchange(true, std::memory_order_acquire)) { M_promise->M_cancel_scheduler->schedule(M_promise->M_cancel_continuation); }
		return M_promise->M_continuation;
	}

	constexpr void await_resume() const noexcept {}
};

template <typename Ret = void>
struct task {
	using Return = std::remove_reference<Ret>::type;
	struct promise_type : public Awaiter_Transforms {
		std::coroutine_handle<> M_continuation;
		Return M_value;
		std::suspend_always initial_suspend() const noexcept
		{
			return {};
		}
		auto final_suspend() noexcept
		{
			return task_final_awaiter<promise_type>(this);
		}

		void return_value(const Return &value) noexcept
		{
			M_value = std::move(value);
		}

		void unhandled_exception() noexcept {}

		task<Return> get_return_object() noexcept
		{
			return task<Return>(this);
		}
	};

	promise_type *M_promise;
	task(promise_type *promise) : M_promise(promise) {}

	task(const task &) = delete;
	task &operator=(const task &) = delete;

	task(task &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
	}
	task &operator=(task &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	auto operator co_await()
	{
		return task_awaiter<promise_type, Return>{M_promise};
	}

	void via(scheduler *s)
	{
		this->M_promise->M_scheduler = s;
	}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<promise_type>(M_promise);
	}
};

template <>
struct task<void> {
	using Return = void;
	struct promise_type : public Awaiter_Transforms {
		std::coroutine_handle<> M_continuation;
		std::suspend_always initial_suspend() const noexcept
		{
			return {};
		}
		auto final_suspend() noexcept
		{
			return task_final_awaiter<promise_type>(this);
		}

		constexpr void return_void() const noexcept {}

		void unhandled_exception() noexcept {}

		task get_return_object() noexcept
		{
			return task(this);
		}
	};

	promise_type *M_promise;
	task(promise_type *promise) : M_promise(promise) {}

	task(const task &) = delete;
	task &operator=(const task &) = delete;

	task(task &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
	}
	task &operator=(task &&Other)
	{
		this->M_promise = Other.M_promise;
		Other.M_promise = nullptr;
		return *this;
	}

	auto operator co_await()
	{
		return task_awaiter<promise_type, void>{M_promise};
	}

	void via(scheduler *s)
	{
		this->M_promise->M_scheduler = s;
	}

	auto cancel()
	{
		M_promise->M_stop_source.request_stop();
		return cancel_awaiter<promise_type>(M_promise);
	}
};

template <typename Awaitable>
async<void> timer(io_service *io, itimerspec spec, Awaitable awaitable)
{
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(tfd, 0, &spec, NULL);
	std::stop_token st = co_await get_stop_token();
	std::stop_callback scb(st, [&] { io->close(tfd); });

	while (!st.stop_requested()) {
		unsigned long u;
		co_await io->read(tfd, &u, sizeof(u), 0);
		co_await awaitable();
	}
	co_await io->close(tfd);
}

template <typename Awaitable>
async<typename Awaitable::Return> delayed(io_service *io, unsigned long sec, unsigned long nsec, Awaitable awaitable)
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
	co_return co_await awaitable;
}

template <typename Return>
async<Return> delayed(io_service *io, unsigned long sec, unsigned long nsec, std::function<Return()> func)
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
	co_return func();
}

async<> delay(io_service *io, unsigned long sec, unsigned long nsec);
} // namespace godby::co
