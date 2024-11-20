#include <array>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <algorithm>
#include <vector>

namespace Nano
{
using Delegate_Key = std::array<std::uintptr_t, 2>;

template <typename RT>
class Function;
template <typename RT, typename... Args>
class Function<RT(Args...)> final {
	// Only Nano::Observer is allowed private access
	template <typename>
	friend class Observer;

	using Thunk = RT (*)(void *, Args &&...);

	static inline Function bind(Delegate_Key const &delegate_key)
	{
		return {reinterpret_cast<void *>(delegate_key[0]), reinterpret_cast<Thunk>(delegate_key[1])};
	}

  public:
	void *const instance_pointer;
	const Thunk function_pointer;

	template <auto fun_ptr>
	static inline Function bind()
	{
		return {nullptr, [](void * /*NULL*/, Args &&...args) { return (*fun_ptr)(std::forward<Args>(args)...); }};
	}

	template <auto mem_ptr, typename T>
	static inline Function bind(T *pointer)
	{
		return {pointer, [](void *this_ptr, Args &&...args) { return (static_cast<T *>(this_ptr)->*mem_ptr)(std::forward<Args>(args)...); }};
	}

	template <typename L>
	static inline Function bind(L *pointer)
	{
		return {pointer, [](void *this_ptr, Args &&...args) { return static_cast<L *>(this_ptr)->operator()(std::forward<Args>(args)...); }};
	}

	template <typename... Uref>
	inline RT operator()(Uref &&...args) const
	{
		return (*function_pointer)(instance_pointer, static_cast<Args &&>(args)...);
	}

	inline operator Delegate_Key() const
	{
		return {reinterpret_cast<std::uintptr_t>(instance_pointer), reinterpret_cast<std::uintptr_t>(function_pointer)};
	}
};

class Spin_Mutex final {
	std::atomic_bool locked = {false};

  public:
	inline void lock() noexcept
	{
		do {
			while (locked.load(std::memory_order_relaxed)) { std::this_thread::yield(); }
		} while (locked.exchange(true, std::memory_order_acquire));
	}

	inline bool try_lock() noexcept
	{
		return !locked.load(std::memory_order_relaxed) && !locked.exchange(true, std::memory_order_acquire);
	}

	inline void unlock() noexcept
	{
		locked.store(false, std::memory_order_release);
	}

	//--------------------------------------------------------------------------

	Spin_Mutex() noexcept = default;
	~Spin_Mutex() noexcept = default;

	// Because all we own is a trivially-copyable atomic_bool, we can manually move/copy
	Spin_Mutex(Spin_Mutex const &other) noexcept : locked(other.locked.load()) {}
	Spin_Mutex &operator=(Spin_Mutex const &other) noexcept
	{
		locked = other.locked.load();
		return *this;
	}

	Spin_Mutex(Spin_Mutex &&other) noexcept : locked(other.locked.load()) {}
	Spin_Mutex &operator=(Spin_Mutex &&other) noexcept
	{
		locked = other.locked.load();
		return *this;
	}
};

//------------------------------------------------------------------------------

/// <summary>
/// Single Thread Policy
/// Use this policy when you DO want performance but NO thread-safety!
/// </summary>
class ST_Policy {
  public:
	template <typename T, typename L>
	inline T const &copy_or_ref(T const &param, L &&) const
	{
		// Return a ref of param
		return param;
	}

	constexpr auto lock_guard() const
	{
		return false;
	}

	constexpr auto scoped_lock(ST_Policy *) const
	{
		return false;
	}

  protected:
	ST_Policy() noexcept = default;
	~ST_Policy() noexcept = default;

	ST_Policy(const ST_Policy &) noexcept = default;
	ST_Policy &operator=(const ST_Policy &) noexcept = default;

	ST_Policy(ST_Policy &&) noexcept = default;
	ST_Policy &operator=(ST_Policy &&) noexcept = default;

	//--------------------------------------------------------------------------

	using Weak_Ptr = ST_Policy *;

	constexpr auto weak_ptr()
	{
		return this;
	}

	constexpr auto observed(Weak_Ptr) const
	{
		return true;
	}

	constexpr auto visiting(Weak_Ptr observer) const
	{
		return (observer == this ? nullptr : observer);
	}

	constexpr auto unmask(Weak_Ptr observer) const
	{
		return observer;
	}

	constexpr void before_disconnect_all() const {}
};

//------------------------------------------------------------------------------

/// <summary>
/// Thread Safe Policy
/// Use this policy when you DO want thread-safety but NO reentrancy!
/// </summary>
/// <typeparam name="Mutex">Defaults to Spin_Mutex</typeparam>
template <typename Mutex = Spin_Mutex>
class TS_Policy {
	mutable Mutex mutex;

  public:
	template <typename T, typename L>
	inline T const &copy_or_ref(T const &param, L &&) const
	{
		// Return a ref of param
		return param;
	}

	inline auto lock_guard() const
	{
		// All policies must implement the BasicLockable requirement
		return std::lock_guard<TS_Policy>(*const_cast<TS_Policy *>(this));
	}

	inline auto scoped_lock(TS_Policy *other) const
	{
		return std::scoped_lock<TS_Policy, TS_Policy>(*const_cast<TS_Policy *>(this), *const_cast<TS_Policy *>(other));
	}

	inline void lock() const
	{
		mutex.lock();
	}

	inline bool try_lock() noexcept
	{
		return mutex.try_lock();
	}

	inline void unlock() noexcept
	{
		mutex.unlock();
	}

  protected:
	TS_Policy() noexcept = default;
	~TS_Policy() noexcept = default;

	TS_Policy(TS_Policy const &) noexcept = default;
	TS_Policy &operator=(TS_Policy const &) noexcept = default;

	TS_Policy(TS_Policy &&) noexcept = default;
	TS_Policy &operator=(TS_Policy &&) noexcept = default;

	//--------------------------------------------------------------------------

	using Weak_Ptr = TS_Policy *;

	constexpr auto weak_ptr()
	{
		return this;
	}

	constexpr auto observed(Weak_Ptr) const
	{
		return true;
	}

	constexpr auto visiting(Weak_Ptr observer) const
	{
		return (observer == this ? nullptr : observer);
	}

	constexpr auto unmask(Weak_Ptr observer) const
	{
		return observer;
	}

	constexpr void before_disconnect_all() const {}
};

//------------------------------------------------------------------------------

/// <summary>
/// Single Thread Policy "Safe"
/// Use this policy when you DO want reentrancy but NO thread-safety!
/// </summary>
class ST_Policy_Safe {
  public:
	template <typename T, typename L>
	inline T copy_or_ref(T const &param, L &&) const
	{
		// Return a copy of param
		return param;
	}

	constexpr auto lock_guard() const
	{
		return false;
	}

	constexpr auto scoped_lock(ST_Policy_Safe *) const
	{
		return false;
	}

  protected:
	ST_Policy_Safe() noexcept = default;
	~ST_Policy_Safe() noexcept = default;

	ST_Policy_Safe(ST_Policy_Safe const &) noexcept = default;
	ST_Policy_Safe &operator=(ST_Policy_Safe const &) noexcept = default;

	ST_Policy_Safe(ST_Policy_Safe &&) noexcept = default;
	ST_Policy_Safe &operator=(ST_Policy_Safe &&) noexcept = default;

	//--------------------------------------------------------------------------

	using Weak_Ptr = ST_Policy_Safe *;

	constexpr auto weak_ptr()
	{
		return this;
	}

	constexpr auto observed(Weak_Ptr) const
	{
		return true;
	}

	constexpr auto visiting(Weak_Ptr observer) const
	{
		return (observer == this ? nullptr : observer);
	}

	constexpr auto unmask(Weak_Ptr observer) const
	{
		return observer;
	}

	constexpr void before_disconnect_all() const {}
};

//------------------------------------------------------------------------------

/// <summary>
/// Thread Safe Policy "Safe"
/// Use this policy when you DO want thread-safety AND reentrancy!
/// </summary>
/// <typeparam name="Mutex">Defaults to Spin_Mutex</typeparam>
template <typename Mutex = Spin_Mutex>
class TS_Policy_Safe {
	using Shared_Ptr = std::shared_ptr<TS_Policy_Safe>;

	Shared_Ptr tracker{this, [](...) {}};
	mutable Mutex mutex;

  public:
	template <typename T, typename L>
	inline T copy_or_ref(T const &param, L &&lock) const
	{
		std::unique_lock<TS_Policy_Safe> unlock_after_copy = std::move(lock);
		// Return a copy of param and then unlock the now "sunk" lock
		return param;
	}

	inline auto lock_guard() const
	{
		// Unique_lock must be used in order to "sink" the lock into copy_or_ref
		return std::unique_lock<TS_Policy_Safe>(*const_cast<TS_Policy_Safe *>(this));
	}

	inline auto scoped_lock(TS_Policy_Safe *other) const
	{
		return std::scoped_lock<TS_Policy_Safe, TS_Policy_Safe>(*const_cast<TS_Policy_Safe *>(this), *const_cast<TS_Policy_Safe *>(other));
	}

	inline void lock() const
	{
		mutex.lock();
	}

	inline bool try_lock() noexcept
	{
		return mutex.try_lock();
	}

	inline void unlock() noexcept
	{
		mutex.unlock();
	}

  protected:
	TS_Policy_Safe() noexcept = default;
	~TS_Policy_Safe() noexcept = default;

	TS_Policy_Safe(TS_Policy_Safe const &) noexcept = default;
	TS_Policy_Safe &operator=(TS_Policy_Safe const &) noexcept = default;

	TS_Policy_Safe(TS_Policy_Safe &&) noexcept = default;
	TS_Policy_Safe &operator=(TS_Policy_Safe &&) noexcept = default;

	//--------------------------------------------------------------------------

	using Weak_Ptr = std::weak_ptr<TS_Policy_Safe>;

	inline Weak_Ptr weak_ptr() const
	{
		return tracker;
	}

	inline Shared_Ptr observed(Weak_Ptr const &observer) const
	{
		return std::move(observer.lock());
	}

	inline Shared_Ptr visiting(Weak_Ptr const &observer) const
	{
		// Lock the observer if the observer isn't tracker
		return observer.owner_before(tracker) || tracker.owner_before(observer) ? std::move(observer.lock()) : nullptr;
	}

	inline auto unmask(Shared_Ptr &observer) const
	{
		return observer.get();
	}

	inline void before_disconnect_all()
	{
		// Immediately create a weak ptr so we can "ping" for expiration
		auto ping = weak_ptr();
		// Reset the tracker and then ping for any lingering refs
		tracker.reset();
		// Wait for all visitors to finish their emissions
		do {
			while (!ping.expired()) { std::this_thread::yield(); }
		} while (ping.lock());
	}
};

template <typename MT_Policy = ST_Policy>
class Observer : private MT_Policy {
	// Only Nano::Signal is allowed private access
	template <typename, typename>
	friend class Signal;

	struct Connection {
		Delegate_Key delegate;
		typename MT_Policy::Weak_Ptr observer;

		Connection() noexcept = default;
		Connection(Delegate_Key const &key) : delegate(key), observer() {}
		Connection(Delegate_Key const &key, Observer *obs) : delegate(key), observer(obs->weak_ptr()) {}
	};

	struct Z_Order {
		inline bool operator()(Delegate_Key const &lhs, Delegate_Key const &rhs) const
		{
			std::size_t x = lhs[0] ^ rhs[0];
			std::size_t y = lhs[1] ^ rhs[1];
			auto k = (x < y) && x < (x ^ y);
			return lhs[k] < rhs[k];
		}

		inline bool operator()(Connection const &lhs, Connection const &rhs) const
		{
			return operator()(lhs.delegate, rhs.delegate);
		}
	};

	std::vector<Connection> connections;

	//--------------------------------------------------------------------------

	void nolock_insert(Delegate_Key const &key, Observer *obs)
	{
		auto begin = std::begin(connections);
		auto end = std::end(connections);

		connections.emplace(std::upper_bound(begin, end, key, Z_Order()), key, obs);
	}

	void insert(Delegate_Key const &key, Observer *obs)
	{
		[[maybe_unused]]
		auto lock = MT_Policy::lock_guard();

		nolock_insert(key, obs);
	}

	void remove(Delegate_Key const &key) noexcept
	{
		[[maybe_unused]]
		auto lock = MT_Policy::lock_guard();

		auto begin = std::begin(connections);
		auto end = std::end(connections);

		auto slots = std::equal_range(begin, end, key, Z_Order());
		connections.erase(slots.first, slots.second);
	}

	//--------------------------------------------------------------------------

	template <typename Function, typename... Uref>
	void for_each(Uref &&...args)
	{
		[[maybe_unused]]
		auto lock = MT_Policy::lock_guard();

		for (auto const &slot : MT_Policy::copy_or_ref(connections, lock)) {
			if (auto observer = MT_Policy::observed(slot.observer)) { Function::bind(slot.delegate)(args...); }
		}
	}

	template <typename Function, typename Accumulate, typename... Uref>
	void for_each_accumulate(Accumulate &&accumulate, Uref &&...args)
	{
		[[maybe_unused]]
		auto lock = MT_Policy::lock_guard();

		for (auto const &slot : MT_Policy::copy_or_ref(connections, lock)) {
			if (auto observer = MT_Policy::observed(slot.observer)) { accumulate(Function::bind(slot.delegate)(args...)); }
		}
	}

	//--------------------------------------------------------------------------

	void nolock_disconnect_all() noexcept
	{
		for (auto const &slot : connections) {
			if (auto observed = MT_Policy::visiting(slot.observer)) {
				auto ptr = static_cast<Observer *>(MT_Policy::unmask(observed));
				ptr->remove(slot.delegate);
			}
		}

		connections.clear();
	}

	void move_connections_from(Observer *other) noexcept
	{
		[[maybe_unused]]
		auto lock = MT_Policy::scoped_lock(other);

		// Make sure this is disconnected and ready to receive
		nolock_disconnect_all();

		// Disconnect other from everyone else and connect them to this
		for (auto const &slot : other->connections) {
			if (auto observed = other->visiting(slot.observer)) {
				auto ptr = static_cast<Observer *>(MT_Policy::unmask(observed));
				ptr->remove(slot.delegate);
				ptr->insert(slot.delegate, this);
				nolock_insert(slot.delegate, ptr);
			}
			// Connect free functions and function objects
			else {
				nolock_insert(slot.delegate, this);
			}
		}

		other->connections.clear();
	}

	//--------------------------------------------------------------------------

  public:
	void disconnect_all() noexcept
	{
		[[maybe_unused]]
		auto lock = MT_Policy::lock_guard();

		nolock_disconnect_all();
	}

	bool is_empty() const noexcept
	{
		[[maybe_unused]]
		auto lock = MT_Policy::lock_guard();

		return connections.empty();
	}

  protected:
	// Guideline #4: A base class destructor should be
	// either public and virtual, or protected and non-virtual.
	~Observer()
	{
		MT_Policy::before_disconnect_all();

		disconnect_all();
	}

	Observer() noexcept = default;

	// Observer may be movable depending on policy, but should never be copied
	Observer(Observer const &) noexcept = delete;
	Observer &operator=(Observer const &) noexcept = delete;

	// When moving an observer, make sure everyone it's connected to knows about it
	Observer(Observer &&other) noexcept
	{
		move_connections_from(std::addressof(other));
	}

	Observer &operator=(Observer &&other) noexcept
	{
		move_connections_from(std::addressof(other));
		return *this;
	}
};

template <typename RT, typename MT_Policy = ST_Policy>
class Signal;

template <typename RT, typename MT_Policy, typename... Args>
class Signal<RT(Args...), MT_Policy> final : public Observer<MT_Policy> {
	using observer = Observer<MT_Policy>;
	using function = Function<RT(Args...)>;

	template <typename T>
	void insert_sfinae(Delegate_Key const &key, typename T::Observer *instance)
	{
		observer::insert(key, instance);
		instance->insert(key, this);
	}
	template <typename T>
	void remove_sfinae(Delegate_Key const &key, typename T::Observer *instance)
	{
		observer::remove(key);
		instance->remove(key);
	}
	template <typename T>
	void insert_sfinae(Delegate_Key const &key, ...)
	{
		observer::insert(key, this);
	}
	template <typename T>
	void remove_sfinae(Delegate_Key const &key, ...)
	{
		observer::remove(key);
	}

  public:
	Signal() noexcept = default;
	~Signal() noexcept = default;

	Signal(Signal const &) noexcept = delete;
	Signal &operator=(Signal const &) noexcept = delete;

	Signal(Signal &&) noexcept = default;
	Signal &operator=(Signal &&) noexcept = default;

	template <typename L>
	void connect(L *instance)
	{
		observer::insert(function::template bind(instance), this);
	}
	template <typename L>
	void connect(L &instance)
	{
		connect(std::addressof(instance));
	}

	template <RT (*fun_ptr)(Args...)>
	void connect()
	{
		observer::insert(function::template bind<fun_ptr>(), this);
	}

	template <typename T, RT (T::*mem_ptr)(Args...)>
	void connect(T *instance)
	{
		insert_sfinae<T>(function::template bind<mem_ptr>(instance), instance);
	}
	template <typename T, RT (T::*mem_ptr)(Args...) const>
	void connect(T *instance)
	{
		insert_sfinae<T>(function::template bind<mem_ptr>(instance), instance);
	}

	template <typename T, RT (T::*mem_ptr)(Args...)>
	void connect(T &instance)
	{
		connect<mem_ptr, T>(std::addressof(instance));
	}
	template <typename T, RT (T::*mem_ptr)(Args...) const>
	void connect(T &instance)
	{
		connect<mem_ptr, T>(std::addressof(instance));
	}

	template <auto mem_ptr, typename T>
	void connect(T *instance)
	{
		insert_sfinae<T>(function::template bind<mem_ptr>(instance), instance);
	}
	template <auto mem_ptr, typename T>
	void connect(T &instance)
	{
		connect<mem_ptr, T>(std::addressof(instance));
	}

	template <typename L>
	void disconnect(L *instance)
	{
		observer::remove(function::template bind(instance));
	}
	template <typename L>
	void disconnect(L &instance)
	{
		disconnect(std::addressof(instance));
	}

	template <RT (*fun_ptr)(Args...)>
	void disconnect()
	{
		observer::remove(function::template bind<fun_ptr>());
	}

	template <typename T, RT (T::*mem_ptr)(Args...)>
	void disconnect(T *instance)
	{
		remove_sfinae<T>(function::template bind<mem_ptr>(instance), instance);
	}
	template <typename T, RT (T::*mem_ptr)(Args...) const>
	void disconnect(T *instance)
	{
		remove_sfinae<T>(function::template bind<mem_ptr>(instance), instance);
	}

	template <typename T, RT (T::*mem_ptr)(Args...)>
	void disconnect(T &instance)
	{
		disconnect<T, mem_ptr>(std::addressof(instance));
	}
	template <typename T, RT (T::*mem_ptr)(Args...) const>
	void disconnect(T &instance)
	{
		disconnect<T, mem_ptr>(std::addressof(instance));
	}

	template <auto mem_ptr, typename T>
	void disconnect(T *instance)
	{
		remove_sfinae<T>(function::template bind<mem_ptr>(instance), instance);
	}
	template <auto mem_ptr, typename T>
	void disconnect(T &instance)
	{
		disconnect<mem_ptr, T>(std::addressof(instance));
	}

	template <typename... Uref>
	void fire(Uref &&...args)
	{
		observer::template for_each<function>(std::forward<Uref>(args)...);
	}

	template <typename Accumulate, typename... Uref>
	void fire_accumulate(Accumulate &&accumulate, Uref &&...args)
	{
		observer::template for_each_accumulate<function, Accumulate>(std::forward<Accumulate>(accumulate), std::forward<Uref>(args)...);
	}
};

} // namespace Nano

//! tests
#include <iostream>
#include <functional>
#include <random>

#define ASSERT_TRUE(condition, message)                                                                                                            \
	do {                                                                                                                                           \
		if (!(condition)) {                                                                                                                        \
			std::cerr << "Assertion failed: (" #condition ") " << "in file " << __FILE__ << ", line " << __LINE__ << ": " << message << std::endl; \
			std::exit(EXIT_FAILURE);                                                                                                               \
		}                                                                                                                                          \
	} while (0)

namespace Nano_Tests
{

namespace
{
static void anonymous_output(const char *fn, const char *sl, std::size_t ln)
{
	std::cout << fn << " LINE: " << __LINE__ << " Test: " << sl << " LINE: " << ln << std::endl;
}
} // namespace

using Rng = std::minstd_rand;

using Observer = Nano::Observer<>;
using Signal_One = Nano::Signal<void(const char *)>;
using Signal_Two = Nano::Signal<void(const char *, std::size_t)>;

using Observer_ST = Nano::Observer<Nano::ST_Policy>;
using Signal_Rng_ST = Nano::Signal<void(Rng &), Nano::ST_Policy>;

using Observer_STS = Nano::Observer<Nano::ST_Policy_Safe>;
using Signal_Rng_STS = Nano::Signal<void(Rng &), Nano::ST_Policy_Safe>;

using Observer_TS = Nano::Observer<Nano::TS_Policy<>>;
using Signal_Rng_TS = Nano::Signal<void(Rng &), Nano::TS_Policy<>>;

using Observer_TSS = Nano::Observer<Nano::TS_Policy_Safe<>>;
using Signal_Rng_TSS = Nano::Signal<void(Rng &), Nano::TS_Policy_Safe<>>;

using Delegate_One = std::function<void(const char *)>;
using Delegate_Two = std::function<void(const char *, std::size_t)>;

//--------------------------------------------------------------------------

class Foo : public Observer {
  public:
	void slot_member_signature_one(const char *sl)
	{
		anonymous_output(__FUNCTION__, sl, __LINE__);
	}
	void slot_member_signature_two(const char *sl, std::size_t ln)
	{
		anonymous_output(__FUNCTION__, sl, ln);
	}

	void slot_const_member_signature_one(const char *sl) const
	{
		anonymous_output(__FUNCTION__, sl, __LINE__);
	}
	void slot_const_member_signature_two(const char *sl, std::size_t ln) const
	{
		anonymous_output(__FUNCTION__, sl, ln);
	}

	void slot_overloaded_member(const char *sl)
	{
		anonymous_output(__FUNCTION__, sl, __LINE__);
	}
	void slot_overloaded_member(const char *sl, std::size_t ln)
	{
		anonymous_output(__FUNCTION__, sl, ln);
	}

	static void slot_static_member_function(const char *sl)
	{
		anonymous_output(__FUNCTION__, sl, __LINE__);
	}
	static void slot_static_member_function(const char *sl, std::size_t ln)
	{
		anonymous_output(__FUNCTION__, sl, ln);
	}

	virtual void slot_virtual_member_function(const char *sl)
	{
		anonymous_output(__FUNCTION__, sl, __LINE__);
	}
	virtual void slot_virtual_member_function(const char *sl, std::size_t ln)
	{
		anonymous_output(__FUNCTION__, sl, ln);
	}
};

//--------------------------------------------------------------------------

class Bar : public Foo {
  public:
	void slot_virtual_member_function(const char *sl) override
	{
		anonymous_output(__FUNCTION__, sl, __LINE__);
	}
	void slot_virtual_member_function(const char *sl, std::size_t ln) override
	{
		anonymous_output(__FUNCTION__, sl, ln);
	}
};

//--------------------------------------------------------------------------

static void slot_static_free_function(const char *sl)
{
	anonymous_output(__FUNCTION__, sl, __LINE__);
}

static void slot_static_free_function(const char *sl, std::size_t ln)
{
	anonymous_output(__FUNCTION__, sl, ln);
}

//--------------------------------------------------------------------------

template <typename T>
class Moo : public T {
  public:
	void slot_next_random(Rng &rng)
	{
		rng.discard(1);
	}

	static void slot_static_next_random(Rng &rng)
	{
		rng.discard(1);
	}
};

static void slot_next_random_free_function(Rng &rng)
{
	rng.discard(1);
}

//--------------------------------------------------------------------------

class Copy_Count {
  public:
	std::size_t count = 0;

	Copy_Count() = default;
	Copy_Count(Copy_Count const &other) : count(other.count + 1) {}
	Copy_Count &operator=(Copy_Count const &other)
	{
		count = other.count + 1;
		return *this;
	}
};
} // namespace Nano_Tests

int main(int argc, char **argv)
{
	using namespace Nano_Tests;

	Signal_One mo_signal_one;
	Signal_Two mo_signal_two;

	Foo mo_foo;
	Bar mo_bar;

	{
		mo_signal_one.connect<&Foo::slot_member_signature_one>(mo_foo);
		mo_signal_two.connect<&Foo::slot_member_signature_two>(mo_foo);

		mo_signal_one.fire(__FUNCTION__);
		mo_signal_two.fire(__FUNCTION__, __LINE__);
	}

	{
		mo_signal_one.connect<&Foo::slot_const_member_signature_one>(mo_foo);
		mo_signal_two.connect<&Foo::slot_const_member_signature_two>(mo_foo);

		mo_signal_one.fire(__FUNCTION__);
		mo_signal_two.fire(__FUNCTION__, __LINE__);
	}

	{
		mo_signal_one.connect<Foo, &Foo::slot_overloaded_member>(mo_foo);
		mo_signal_two.connect<Foo, &Foo::slot_overloaded_member>(mo_foo);

		mo_signal_one.fire(__FUNCTION__);
		mo_signal_two.fire(__FUNCTION__, __LINE__);
	}

	{
		mo_signal_one.connect<&Foo::slot_static_member_function>();
		mo_signal_two.connect<&Foo::slot_static_member_function>();


		mo_signal_one.fire(__FUNCTION__);
		mo_signal_two.fire(__FUNCTION__, __LINE__);
	}

	{
		mo_signal_one.connect<Foo, &Foo::slot_virtual_member_function>(mo_foo);
		mo_signal_two.connect<Foo, &Foo::slot_virtual_member_function>(mo_foo);

		mo_signal_one.fire(__FUNCTION__);
		mo_signal_two.fire(__FUNCTION__, __LINE__);
	}

	{
		mo_signal_one.connect<Bar, &Bar::slot_virtual_member_function>(mo_bar);
		mo_signal_two.connect<Bar, &Bar::slot_virtual_member_function>(mo_bar);

		mo_signal_one.fire(__FUNCTION__);
		mo_signal_two.fire(__FUNCTION__, __LINE__);
	}

	{
		Nano::Signal<std::size_t(std::size_t)> signal_three;
		auto slot_three = [&](std::size_t val) { return val * val; };
		signal_three.connect(slot_three);

		std::vector<std::size_t> signal_return_values;
		auto accumulator = [&](std::size_t srv) { signal_return_values.push_back(srv); };

		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);
		signal_three.fire_accumulate(accumulator, __LINE__);

		ASSERT_TRUE(signal_return_values.size() == 10, "An SRV was found missing.");
	}

	{
		Nano::Signal<void(Copy_Count)> signal_one;

		auto slot_one = [](Copy_Count cc) { ASSERT_TRUE(cc.count == 1, "A parameter was copied more than once."); };

		signal_one.connect(slot_one);

		auto slot_two = [](Copy_Count cc) { ASSERT_TRUE(cc.count == 1, "A parameter was copied more than once."); };

		signal_one.connect(slot_two);

		Copy_Count cc;

		signal_one.fire(cc);
	}

	{
		Nano::Signal<void(Copy_Count &)> signal_one;

		auto slot_one = [](Copy_Count &cc) { ASSERT_TRUE(cc.count == 0, "A reference parameter was copied."); };

		signal_one.connect(slot_one);

		auto slot_two = [](Copy_Count &cc) { ASSERT_TRUE(cc.count == 0, "A reference parameter was copied."); };

		signal_one.connect(slot_two);

		Copy_Count cc;

		signal_one.fire(cc);
	}

	{
		Nano::Signal<void(Copy_Count)> signal_one;

		auto slot_one = [](Copy_Count cc) { ASSERT_TRUE(cc.count == 1, "An rvalue parameter wasn't copied once."); };

		signal_one.connect(slot_one);

		auto slot_two = [](Copy_Count cc) { ASSERT_TRUE(cc.count == 1, "An rvalue parameter wasn't copied once."); };

		signal_one.connect(slot_two);

		signal_one.fire(Copy_Count());
	}
}
