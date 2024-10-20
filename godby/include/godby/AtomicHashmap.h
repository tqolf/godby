#pragma once

#include <utility>		  // std::forward
#include <cmath>		  // std::log
#include <cstdlib>		  // std::abort
#include <functional>	  // std::function
#include <stdexcept>	  // std::runtime_error
#include <vector>		  // std::vector
#include <godby/Atomic.h> // godby::Atomic
#include <godby/Math.h>	  // godby::NextPrime

static_assert(__cplusplus >= 202002L, "Requires C++20 or higher");

//! AtomicHashmap
namespace godby
{
namespace details
{
struct key_tag {};
struct value_tag {};

template <typename T>
struct Referenced {
	int __ref;
	T __value;
	using Composed = Referenced<T>;
	Referenced() : __ref(1) {}

	static Composed *clone(const T &other)
	{
		auto *composed = new Composed;
		if (composed != nullptr) { composed->__value = other; }

		return composed;
	}

	static Composed *clone(T &&other)
	{
		auto *composed = new Composed;
		if (composed != nullptr) {
			new (composed) Composed();
			composed->__value = std::forward<T>(other);
		}

		return composed;
	}

	static void deallocate(Composed *composed)
	{
		if (composed && composed->Release()) { delete composed; }
	}

	inline bool Acquire()
	{
		int curr = __atomic_load_n(&__ref, __ATOMIC_RELAXED);
		while (true) {
			if (curr == 0) {
				// Refcount already hit zero. Destructor is already running so we
				// can't revive the object.
				return false;
			}

			if (__atomic_compare_exchange_n(&__ref, &curr, curr + 1, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
				// Successfully incremented refcount without letting it hit zero.
				return true;
			}
		}
	}
	inline bool Release()
	{
		if (__atomic_sub_fetch(&__ref, 1, __ATOMIC_RELEASE) == 0) {
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			return true;
		}
		return false;
	}

	struct Accessor {
		Composed *M_compoesed = nullptr;

		Accessor(Composed *composed) noexcept
		{
			if (composed && composed->Acquire()) { M_compoesed = composed; }
		}

		Accessor(Accessor &other) noexcept
		{
			if (other.M_compoesed && other.M_compoesed->Acquire()) { M_compoesed = other.M_compoesed; }
		}

		Accessor(Accessor &&other) noexcept
		{
			M_compoesed = other.M_compoesed;

			other.M_compoesed = nullptr;
		}

		inline Composed *composed() noexcept
		{
			return M_compoesed;
		}

		inline bool has() noexcept
		{
			return M_compoesed != nullptr;
		}

		inline operator bool() noexcept
		{
			return M_compoesed != nullptr;
		}

		inline T &value() noexcept
		{
			return M_compoesed->__value;
		}

		~Accessor()
		{
			if (M_compoesed && M_compoesed->Release()) {
				delete M_compoesed;
				M_compoesed = nullptr;
			}
		}
	};
};

template <typename Key, typename Value>
struct ConcurrentMapBucket {
	size_t key = 0;
	size_t value = 0;

	using ComposedKey = Referenced<Key>;
	using ComposedValue = Referenced<Value>;
	using KeyAccessor = typename ComposedKey::Accessor;
	using ValueAccessor = typename ComposedValue::Accessor;

	ConcurrentMapBucket() = default;

	inline KeyAccessor AccessKey() noexcept
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(key), &key));
		size_t off = __atomic_load_n(&key, __ATOMIC_RELAXED);
		return KeyAccessor(reinterpret_cast<ComposedKey *>(uintptr_t(off)));
	}

	inline ValueAccessor AccessValue() noexcept
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(value), &value));
		size_t off = __atomic_load_n(&value, __ATOMIC_RELAXED);
		return ValueAccessor(reinterpret_cast<ComposedValue *>(uintptr_t(off)));
	}

	template <typename tag, typename Composed>
	inline bool Exchange(Composed *composed, Composed *&oldcomposed)
	{
		return ExchangeImpl(tag{}, composed, oldcomposed);
	}

	template <typename tag, typename Composed>
	inline bool Exchange(Composed *composed)
	{
		Composed *oldcomposed = nullptr;
		if (Exchange<tag, Composed>(composed, oldcomposed)) {
			Composed::deallocate(oldcomposed);
			return true;
		}

		return false;
	}

	inline void Swap(ConcurrentMapBucket *bucket) noexcept
	{
		std::swap(key, bucket->key);
		std::swap(value, bucket->value);
	}

	inline void Cleanup() noexcept
	{
		Exchange<key_tag, ComposedKey>(nullptr);
		Exchange<value_tag, ComposedValue>(nullptr);
	}

	inline bool IsOccupied() noexcept
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(key), &key));
		return __atomic_load_n(&key, __ATOMIC_RELAXED) != 0;
	}

	inline bool IsOccupied(const Key &rhs) noexcept
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(key), &key));
		auto accessor = AccessKey();
		return accessor.has() && accessor.value() == rhs;
	}

	inline bool IsAvailable(const Key &rhs) noexcept
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(key), &key));
		auto accessor = AccessKey();
		return !accessor.has() || accessor.value() == rhs;
	}

  private:
	template <typename tag, typename Composed>
	inline bool ExchangeImpl(tag, Composed *composed, Composed *&oldcomposed)
	{
		std::abort();
		return false;
	}

	inline bool ExchangeImpl(key_tag, ComposedKey *composed, ComposedKey *&oldcomposed)
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(key), &key));
		size_t off = size_t(uintptr_t(composed));
		size_t orig = __atomic_load_n(&key, __ATOMIC_RELAXED);
		oldcomposed = reinterpret_cast<ComposedKey *>(uintptr_t(orig));
		return __atomic_compare_exchange_n(&key, &orig, off, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
	}

	inline bool ExchangeImpl(value_tag, ComposedValue *composed, ComposedValue *&oldcomposed)
	{
		GODBY_ASSERT(__atomic_is_lock_free(sizeof(value), &value));
		size_t off = size_t(uintptr_t(composed));
		size_t orig = __atomic_load_n(&value, __ATOMIC_RELAXED);
		oldcomposed = reinterpret_cast<ComposedValue *>(uintptr_t(orig));
		if (__atomic_compare_exchange_n(&value, &orig, off, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { return true; }

		return false;
	}
};
} // namespace details

template <typename Key, typename Value, typename Hasher = std::hash<Key>>
class AtomicHashmap {
  public:
	using Bucket = details::ConcurrentMapBucket<Key, Value>;
	using ComposedKey = typename Bucket::ComposedKey;
	using ComposedValue = typename Bucket::ComposedValue;
	using KeyAccessor = typename Bucket::KeyAccessor;
	using ValueAccessor = typename Bucket::ValueAccessor;

	AtomicHashmap(size_t expected_capacity, size_t level_size = 13) : M_capacity(0), M_level_size(level_size)
	{
		if (GenMultiLevelSize(expected_capacity, level_size, M_level_capacity, M_capacity) != 0) { throw std::runtime_error("Invalid capacity or level-size"); }
		size_t n = (unsigned long long)(expected_capacity)*expected_capacity / M_capacity;
		if (GenMultiLevelSize(n, level_size, M_level_capacity, M_capacity) != 0) { throw std::runtime_error("Invalid capacity or level-size"); }

		M_buckets = new Bucket[M_capacity];
	}

	~AtomicHashmap()
	{
		if (M_buckets) { delete[] M_buckets; }
	}

	AtomicHashmap(AtomicHashmap &other) = delete;
	AtomicHashmap(AtomicHashmap &&other) = delete;
	AtomicHashmap &operator=(AtomicHashmap &other) = delete;
	AtomicHashmap &operator=(AtomicHashmap &&other) = delete;

	ValueAccessor Get(const Key &key) noexcept
	{
		Bucket *bucket = Lookup(key);
		return bucket ? bucket->AccessValue() : ValueAccessor{nullptr};
	}

	// <0, error code
	int Set(const Key &key, const Value &value) noexcept
	{
		Bucket *bucket = Occupy(key);
		if (bucket == nullptr) { return -ENOENT; }

		ComposedValue *composed = Bucket::ComposedValue::clone(value);
		if (composed == nullptr) {
			// TRACE("Out of memory");
			return -ENOMEM;
		}
		bucket->template Exchange<details::value_tag>(composed);
		return 0;
	}

	int Delete(const Key &key) noexcept
	{
		WalkKey(key, [](Bucket *bucket) { bucket->Cleanup(); });
		return 0;
	}

	inline bool IsOccupied(Bucket *bucket)
	{
		return bucket->IsOccupied();
	}

	inline bool IsOccupied(Bucket *bucket, const Key &key)
	{
		return bucket->IsOccupied(key);
	}

	inline bool IsAvailable(Bucket *bucket, const Key &key)
	{
		return bucket->IsAvailable(key);
	}

	inline void Cleanup()
	{
		for (size_t i = 0; i < M_capacity; ++i) {
			Bucket *bucket = M_buckets + i;
			if (bucket->IsOccupied()) { bucket->Cleanup(); }
		}
	}

	inline void Cleanup(Bucket *bucket)
	{
		if (bucket && bucket->IsOccupied()) { bucket->Cleanup(); }
	}

	void WalkAll(std::function<void(Bucket *)> walker)
	{
		for (size_t i = 0; i < M_capacity; ++i) {
			Bucket *bucket = M_buckets + i;
			if (bucket->IsOccupied()) { walker(bucket); }
		}
	}

	void WalkKey(const Key &key, std::function<void(Bucket *)> walker)
	{
		for (size_t i = 0; i < M_capacity; ++i) {
			Bucket *bucket = M_buckets + i;
			if (bucket->IsOccupied(key)) { walker(bucket); }
		}
	}

	void WalkAll(std::function<void(KeyAccessor key, ValueAccessor value)> walker)
	{
		for (size_t i = 0; i < M_capacity; ++i) {
			Bucket *bucket = M_buckets + i;
			if (bucket->IsOccupied()) { walker(bucket->AccessKey(), bucket->AccessValue()); }
		}
	}

	void WalkKey(const Key &key, std::function<void(KeyAccessor key, ValueAccessor value)> walker)
	{
		for (size_t i = 0; i < M_capacity; ++i) {
			Bucket *bucket = M_buckets + i;
			if (bucket->IsOccupied(key)) { walker(bucket->AccessKey(), bucket->AccessValue()); }
		}
	}

	class iterator {
	  public:
		iterator(Bucket *current, Bucket *end) : current(current), end(end)
		{
			advance_to_next_valid();
		}

		Bucket &operator*() const
		{
			return *current;
		}

		iterator &operator++()
		{
			++current;
			advance_to_next_valid();
			return *this;
		}

		iterator operator++(int)
		{
			iterator temp = *this;
			++(*this);
			return temp;
		}
		bool operator==(const iterator &other) const
		{
			return current == other.current;
		}

		bool operator!=(const iterator &other) const
		{
			return !(*this == other);
		}

	  private:
		Bucket *current, *end;

		inline void advance_to_next_valid()
		{
			while (current != end && !current->IsOccupied()) { ++current; }
		}
	};

	iterator begin()
	{
		return iterator(M_buckets, M_buckets + M_capacity);
	}

	iterator end()
	{
		return iterator(M_buckets + M_capacity, M_buckets + M_capacity);
	}

  protected:
	size_t M_capacity;
	size_t M_level_size;
	std::vector<size_t> M_level_capacity;
	Bucket *M_buckets = nullptr;

	static int GenMultiLevelSize(size_t n, size_t level, std::vector<size_t> &capacities, size_t &sum)
	{
		static const double occupied_ratio = 0.989;
		static const double ln_ratio = -std::log(1 - occupied_ratio);

		sum = 0;
		capacities.clear();
		for (size_t i = 0; i < level; ++i) {
			capacities.push_back(godby::NextPrime(1. * n / ln_ratio));
			sum += capacities.back();
			size_t occupied = capacities.back() * occupied_ratio;
			if (n < occupied) { return -1; }
			n -= capacities.back() * occupied_ratio;
		}
		return 0;
	}

	inline Bucket *Lookup(const Key &key)
	{
		size_t hash = Hasher()(key);
		Bucket *base = this->M_buckets;
		for (size_t i = 0; i < M_level_capacity.size(); ++i) {
			size_t capacity = M_level_capacity[i];
			Bucket *bucket = base + (hash % capacity);
			if (this->IsOccupied(bucket, key)) { return bucket; }
			base += capacity;
		}

		return nullptr;
	}

	inline Bucket *Occupy(const Key &key)
	{
		size_t hash = Hasher()(key);
		Bucket *base = this->M_buckets;
		ComposedKey *composed = nullptr;
		for (size_t i = 0; i < M_level_capacity.size(); ++i) {
			size_t capacity = M_level_capacity[i];
			Bucket *bucket = base + (hash % capacity);
			if (this->IsAvailable(bucket, key)) {
				if (composed == nullptr) {
					composed = Bucket::ComposedKey::clone(key);
					if (composed == nullptr) {
						// TRACE("Out of memory");
						return nullptr;
					}
				}
				if (bucket->template Exchange<details::key_tag>(composed)) { return bucket; }
			}
			base += capacity;
		}
		if (composed != nullptr) { Bucket::ComposedKey::deallocate(composed); }

		return nullptr;
	}
};
} // namespace godby
