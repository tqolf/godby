#pragma once

#include <cstddef>				 // std::size_t
#include <atomic>				 // std::atomic
#include <thread>				 // std::this_thread
#include <utility>				 // std::forward
#include <vector>				 // std::vector
#include <mutex>				 // std::mutex
#include <condition_variable>	 // std::condition_variable
#include <godby/Atomic.h>		 // godby::Atomic
#include <godby/Math.h>			 // godby::NextPowerOfTwo
#include <godby/Spinlock.h>		 // godby::Spinlock
#include <godby/Signal.h>		 // godby::Signal
#include <godby/StealingQueue.h> // godby::StealingQueue

static_assert(__cplusplus >= 202002L, "Requires C++20 or higher");

//! StealingExecutor
namespace godby
{
namespace details
{
class NonLock final {
  public:
	inline void lock() {}
	inline void unlock() {}
};
} // namespace details

class WaitGroup {
  public:
	// Constructs the WaitGroup with the specified initial count.
	inline WaitGroup(unsigned int initialCount = 0)
	{
		data->count = initialCount;
	}

	// add() increments the internal counter by count.
	inline void add(unsigned int count = 1) const
	{
		data->count += count;
	}

	// done() decrements the internal counter by one.
	// Returns true if the internal count has reached zero.
	inline bool done() const
	{
		GODBY_ASSERT(data->count > 0 && "WaitGroup::done() called too many times");
		auto count = --data->count;
		if (count == 0) {
			std::unique_lock<std::mutex> lock(data->mutex);
			data->condition.notify_all();
			return true;
		}
		return false;
	}

	// wait() blocks until the WaitGroup counter reaches zero.
	inline void wait() const
	{
		std::unique_lock<std::mutex> lock(data->mutex);
		data->condition.wait(lock, [this] { return data->count.load(std::memory_order_relaxed) == 0; });
	}

  protected:
	struct Data {
		std::atomic<int> count = {0};
		std::condition_variable condition;
		std::mutex mutex;
	};
	const std::shared_ptr<Data> data = std::make_shared<Data>();
};

/**
 * @class: StealingExecutor
 *
 * @tparam T data type
 * @tparam Policy executor policy
 *
 * @brief: unbounded stealing-queue based multi-threading executor
 *
 * This class implements the work stealing queue based multi-threading pool.
 *
 * Only the executor owner can perform Submit operation.
 */

template <size_t MaxStealSize = 10, size_t PauseCheckGap = 4>
struct StealingConfig {
	static constexpr size_t max_steal_size = MaxStealSize;
	static constexpr size_t pause_check_mask = godby::NextPowerOfTwo(PauseCheckGap) - 1;
};

template <bool Waitable = false, bool Sharing = false, typename Waiter = WaitGroup, typename ThreadType = std::thread>
struct StealingPolicy {
	static constexpr bool sharing = Sharing;
	static constexpr bool waitable = Waitable;

	using waiter_type = Waiter;
	using thread_type = ThreadType;

	template <typename T>
	using composed_type = typename std::conditional<waitable, std::tuple<waiter_type *, T>, T>::type;
};
using DefaultStealingConfig = StealingConfig<10, 4>;
using DefaultStealingPolicy = StealingPolicy<false, false>;

template <typename T, typename Policy = DefaultStealingPolicy, typename Config = DefaultStealingConfig>
class StealingExecutor {
  public:
	using SUPER = StealingExecutor;

	using value_type = T;
	using waiter_type = typename Policy::waiter_type;
	using composed_type = typename Policy::template composed_type<value_type>;

	class Controller {
	  public:
		static inline void Cancel()
		{
			Instance()._Cancel();
		}
		static inline bool Cancelled()
		{
			return Instance()._Cancelled();
		}
		static inline bool Cancellable()
		{
			return Instance()._Cancellable();
		}

	  protected:
		int M_sigid;
		bool M_cancelled;

		static Controller &Instance()
		{
			static Controller instance;
			return instance;
		}

		Controller() : M_sigid(-1), M_cancelled(false) {}
		~Controller()
		{
			if (M_sigid >= 0) {
				Signal::Unregister(M_sigid);
				M_sigid = -1;
			}
		}

		inline void _Cancel()
		{
			M_cancelled = true;
		}
		inline bool _Cancelled()
		{
			return M_cancelled;
		}
		inline bool _Cancellable()
		{
			if (M_sigid >= 0) {
				Signal::Unregister(M_sigid);
				M_sigid = -1;
			}
			M_sigid = Signal::Register(SIGINT, [&]() { M_cancelled = true; });

			return Signal::Install(SIGINT) == 0;
		}
	};

	StealingExecutor(size_t queue_capacity = 1024)
		: M_pause{false},
		  M_shutdown{false},
		  M_should_notify{true},
		  M_done_spinlock{false},
		  M_owner(std::this_thread::get_id()),
		  M_working_size{0},
		  M_waiting_size{0},
		  M_initialized_size{0},
		  M_queue(godby::NextPowerOfTwo(queue_capacity))
	{
	}

	virtual ~StealingExecutor()
	{
		Shutdown();
	}

	void Spawn(size_t concurrency)
	{
		for (size_t i = 0; i < concurrency; ++i) {
			// create consumer threads
			M_consumer_threads.emplace_back(&StealingExecutor::Consumer, this, i);
		}

		// spin lock until all threads are created (required for synchronization)
		while (M_waiting_size.load() != concurrency) { std::this_thread::yield(); }

		Resume();
	}

	void Pause()
	{
		M_pause.store(true);
	}

	void Resume()
	{
		M_pause.store(false);
		M_should_notify.store(false);

		size_t wqsize = M_queue.size();
		if (wqsize >= M_consumer_threads.size()) {
			M_wakeup_cond.notify_all();
		} else {
			for (size_t i = 0; i < wqsize; i += 1) { M_wakeup_cond.notify_one(); }
		}
	}

	void WaitAll()
	{
		M_should_notify.store(true);

		// tell threads to begin working
		size_t wqsize = M_queue.size();
		if (wqsize >= M_consumer_threads.size()) {
			M_wakeup_cond.notify_all();
		} else {
			for (size_t i = 0; i < wqsize; i += 1) { M_wakeup_cond.notify_one(); }
		}

		// sleep main thread until work is done
		std::unique_lock<std::mutex> suspend_latch(M_suspend_mutex, std::defer_lock);
		suspend_latch.lock();

		while ((!M_pause.load() && !M_queue.empty()) || M_working_size.load() != 0) { M_suspend_cond.wait(suspend_latch); }
		suspend_latch.unlock();

		// notify the notifying thread that we are awakened
		M_done_spinlock.store(true);

		// wait until all threads have gone back to the waiting state
		while (M_waiting_size.load() != M_consumer_threads.size()) { std::this_thread::yield(); }

		// reset previous spin lock state
		M_done_spinlock.store(false);
	}

	void Shutdown()
	{
		if (M_shutdown.load()) { return; }

		WaitAll();

		// wait until all threads are waiting
		while (M_waiting_size.load() != M_consumer_threads.size()) {}
		M_shutdown.store(true);

		// spin lock until all threads are going to quit, and spam notify to
		// make sure they all get the message
		while (M_initialized_size.load() != 0) { M_wakeup_cond.notify_all(); }

		for (auto &consumer : M_consumer_threads) {
			if (consumer.joinable()) { consumer.join(); }
		}
		M_consumer_threads.clear();

		M_shutdown.store(false);
	}

	void Submit(const value_type &value)
	{
		{
			M_owner_lock.lock();
			if constexpr (Policy::waitable) {
				M_queue.push(std::make_tuple(nullptr, value));
			} else {
				M_queue.push(value);
			}
			M_owner_lock.unlock();
		}

		if (!M_pause.load(std::memory_order_relaxed) && M_consumer_threads.size() - M_waiting_size.load(std::memory_order_relaxed) <= M_min_active_consumer) {
			// No consumers are consuming, so wake one up
			std::lock_guard<std::mutex> lock(M_wakeup_mutex);
			M_wakeup_cond.notify_one();
		}
	}

	void Submit(value_type &&value)
	{
		{
			M_owner_lock.lock();
			if constexpr (Policy::waitable) {
				M_queue.push(std::make_tuple(nullptr, std::forward<value_type>(value)));
			} else {
				M_queue.push(std::forward<value_type>(value));
			}
			M_owner_lock.unlock();
		}

		if (!M_pause.load(std::memory_order_relaxed) && M_consumer_threads.size() - M_waiting_size.load(std::memory_order_relaxed) <= M_min_active_consumer) {
			// No consumers are consuming, so wake one up
			std::lock_guard<std::mutex> lock(M_wakeup_mutex);
			M_wakeup_cond.notify_one();
		}
	}

	template <bool Waitable = Policy::waitable>
	typename std::enable_if<Waitable, void>::type Submit(waiter_type &waiter, const value_type &value)
	{
		{
			waiter.add();
			M_owner_lock.lock();
			M_queue.push(std::make_tuple(&waiter, value));
			M_owner_lock.unlock();
		}

		if (!M_pause.load(std::memory_order_relaxed) && M_consumer_threads.size() - M_waiting_size.load(std::memory_order_relaxed) <= M_min_active_consumer) {
			// No consumers are consuming, so wake one up
			std::lock_guard<std::mutex> lock(M_wakeup_mutex);
			M_wakeup_cond.notify_one();
		}
	}

	template <bool Waitable = Policy::waitable>
	typename std::enable_if<Waitable, void>::type Submit(waiter_type &waiter, value_type &&value)
	{
		{
			waiter.add();
			M_owner_lock.lock();
			M_queue.push(std::make_tuple(&waiter, std::forward<value_type>(value)));
			M_owner_lock.unlock();
		}

		if (!M_pause.load(std::memory_order_relaxed) && M_consumer_threads.size() - M_waiting_size.load(std::memory_order_relaxed) <= M_min_active_consumer) {
			// No consumers are consuming, so wake one up
			std::lock_guard<std::mutex> lock(M_wakeup_mutex);
			M_wakeup_cond.notify_one();
		}
	}

	template <typename Iterator, bool Waitable = Policy::waitable>
	inline typename std::enable_if<Waitable, void>::type Parallelize(Iterator begin, Iterator end)
	{
		waiter_type waiter;
		for (auto it = begin; it != end; ++it) { Submit(waiter, *it); }
		waiter.wait();
	}

	void Purge()
	{
		M_owner_lock.lock();
		while (!M_queue.empty()) { M_queue.pop(); }
		M_owner_lock.unlock();
	}

	inline bool IsEmpty()
	{
		return M_queue.empty() && M_working_size.load() == 0;
	}

	inline size_t QueueSize() const
	{
		return M_queue.size();
	}

  protected:
	std::atomic<bool> M_pause;
	std::atomic<bool> M_shutdown;
	std::atomic<bool> M_should_notify;
	std::atomic<bool> M_done_spinlock;

	std::thread::id M_owner;
	std::atomic<size_t> M_working_size;
	std::atomic<size_t> M_waiting_size;
	std::atomic<size_t> M_initialized_size;
	std::vector<typename Policy::thread_type> M_consumer_threads;

	size_t M_min_active_consumer = 3;

	template <bool Enabled>
	using ConditionalSpinlock = std::conditional_t<Enabled, godby::Spinlock, details::NonLock>;
	ConditionalSpinlock<Policy::sharing> M_owner_lock;

	std::mutex M_wakeup_mutex;
	std::condition_variable M_wakeup_cond;

	std::mutex M_suspend_mutex;
	std::condition_variable M_suspend_cond;

	StealingQueue<composed_type> M_queue;

	virtual void Consume(value_type &&) {}

	virtual void Callback_Setup(size_t /* cid */) {}
	virtual void Callback_Teardown(size_t /* cid */) {}

	virtual void Callback_Wakeup(size_t /* cid */) {}
	virtual void Callback_Suspend(size_t /* cid */, size_t /* nth */) {}

	virtual bool Callback_IsLimited(size_t /* cid */, size_t /* nth */)
	{
		return false;
	}

	virtual void Callback_Consume(size_t /* cid */, size_t /* nth */, value_type &&value)
	{
		Consume(std::forward<value_type>(value));
	}
	virtual void Callback_Cleanup(size_t /* cid */, size_t /* nth */, value_type && /* value */) {}

	template <typename U = composed_type, bool enabled = !std::is_same<U, value_type>::value>
	inline typename std::enable_if<enabled, void>::type Callback_Consume(size_t cid, size_t nth, U &&value)
	{
		waiter_type *waiter = std::get<0>(value);
		Callback_Consume(cid, nth, std::move(std::get<1>(value)));
		if (waiter) { waiter->done(); }
	}

	template <typename U = composed_type, bool enabled = !std::is_same<U, value_type>::value>
	inline typename std::enable_if<enabled, void>::type Callback_Cleanup(size_t cid, size_t nth, U &&value)
	{
		waiter_type *waiter = std::get<0>(value);
		Callback_Cleanup(cid, nth, std::move(std::get<1>(value)));
		if (waiter) { waiter->done(); }
	}

	virtual void Callback_Execute(size_t cid, size_t &nth)
	{
		auto work = M_queue.steal();
		if (work.has_value()) {
			++nth;
			M_working_size.fetch_add(1);

			if (Controller::Cancelled()) {
				Callback_Cleanup(cid, nth, std::move(work.value()));
			} else {
				Callback_Consume(cid, nth, std::move(work.value()));
			}

			work = M_queue.steal();
			while (work.has_value()) {
				++nth;

				if (Controller::Cancelled()) {
					Callback_Cleanup(cid, nth, std::move(work.value()));
				} else {
					Callback_Consume(cid, nth, std::move(work.value()));
					if (Config::max_steal_size != 0 && nth >= Config::max_steal_size) { break; }
					if ((nth & Config::pause_check_mask) == 0 && M_pause.load(std::memory_order_relaxed)) { break; }
					if (Callback_IsLimited(cid, nth)) { break; }
				}

				work = M_queue.steal();
			}

			M_working_size.fetch_sub(1);
		}
	}

	virtual void Consumer(size_t cid)
	{
		Callback_Setup(cid);
		M_initialized_size.fetch_add(1);

		std::unique_lock<std::mutex> wakeup_latch(M_wakeup_mutex, std::defer_lock);

		while (true) {
			wakeup_latch.lock();
			M_waiting_size.fetch_add(1); // tell main thread we are waiting

			// sleep until we have work to do or we need to exit
			while (M_queue.empty() && !M_shutdown.load()) { M_wakeup_cond.wait(wakeup_latch); }

			wakeup_latch.unlock();
			M_waiting_size.fetch_sub(1); // tell main thread we are no longer waiting

			// exit the thread
			if (M_shutdown.load()) { break; }

			size_t nth = 0;
			Callback_Wakeup(cid);

			// grab the queued while there is queue remaining and consume it
			Callback_Execute(cid, nth);

			Callback_Suspend(cid, nth);

			// if we are the last thread to finish, tell the main thread that
			// all threads have finished
			if (M_queue.empty() && M_working_size.load() == 0) {
				// spin lock until we have confirmation from the main thread that
				// it knows we are done working
				while (M_should_notify.load() && !M_done_spinlock.load() && M_working_size.load() == 0 && M_queue.empty()) { M_suspend_cond.notify_all(); }
			}
		}

		Callback_Teardown(cid);
		M_initialized_size.fetch_sub(1);
	}
};
} // namespace godby
