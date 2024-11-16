#pragma once

#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <thread>

#include <godby/TimerWheel.h>

#ifndef TRACE
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#define TOSTRING(line)		 #line
#define LOCATION(file, line) &file ":" TOSTRING(line)[(__builtin_strrchr(file, '/') ? (__builtin_strrchr(file, '/') - file + 1) : 0)]

#define TRACE(fmt, ...)                                                                                 \
	do {                                                                                                \
		char buff[32];                                                                                  \
		struct tm tm;                                                                                   \
		struct timeval tv;                                                                              \
		gettimeofday(&tv, NULL);                                                                        \
		localtime_r(&tv.tv_sec, &tm);                                                                   \
		size_t len = strftime(buff, sizeof(buff), "%Y-%m-%d %H:%M:%S", &tm);                            \
		snprintf(&buff[len], sizeof(buff) - len, ".%03d", (int)(((tv.tv_usec + 500) / 1000) % 1000));   \
		printf("\033[2;3m%s\033[0m <%s> " fmt "\n", buff, LOCATION(__FILE__, __LINE__), ##__VA_ARGS__); \
	} while (0);
#endif

namespace godby
{
constexpr bool ConstexprIsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

template <typename T>
constexpr std::string_view ConstexprGetType()
{
	char const *info = __PRETTY_FUNCTION__;
	std::string_view name = [&] {
		std::size_t l = strlen(info);
		std::size_t equals = 0, semicolon = 0, end = 0;
		for (std::size_t i = 0; i < l && !end; ++i) {
			switch (info[i]) {
				case '=':
					if (!equals) { equals = i; }
					break;
				case ';':
					semicolon = i;
					break;
				case ']':
					end = i;
					break;
			}
		}
#if defined(__clang__) && defined(__APPLE__)
		semicolon = l - 1;
#endif

		if (equals && semicolon && end) {
			size_t start = equals + 1;
			end = semicolon - equals - 1 + start;
			while (start < end && ConstexprIsSpace(info[start])) { ++start; }
			while (end > start && ConstexprIsSpace(info[end - 1])) { --end; }
			return std::string_view(info + start, end - start);
		} else {
			return std::string_view("");
		}
	}();

	return name;
}

template <typename T, typename... Tags>
std::string MakeKey(Tags &&...tags)
{
	std::ostringstream oss;
	oss << ConstexprGetType<T>() << "::";
	((oss << std::forward<Tags>(tags) << '.'), ...);

	std::string key = oss.str();
	if (!key.empty() && key.back() == '.') { key.pop_back(); }

	return key;
}

#define INC(name) name.fetch_add(1, std::memory_order_relaxed)

std::atomic<size_t> local_hits{0};
std::atomic<size_t> local_misses{0};
std::atomic<size_t> shared_hits{0};
std::atomic<size_t> shared_misses{0};

template <typename T>
class ThreadLocalCache {
  public:
	static inline std::shared_ptr<T> Get(const std::string &key)
	{
		auto &ins = Singlon();
		auto it = ins.cached.find(key);
		return (it != ins.cached.end()) ? (INC(local_hits), it->second) : (INC(local_misses), nullptr);
	}

	static inline void Set(const std::string &key, std::shared_ptr<T> value)
	{
		auto &ins = Singlon();
		ins.cached[key] = std::move(value);
	}

	static inline bool Has(const std::string &key)
	{
		auto &ins = Singlon();
		return ins.cached.find(key) != ins.cached.end();
	}

  private:
	ThreadLocalCache() = default;

	ThreadLocalCache(const ThreadLocalCache &) = delete;
	ThreadLocalCache &operator=(const ThreadLocalCache &) = delete;

	ThreadLocalCache(ThreadLocalCache &&) = delete;
	ThreadLocalCache &operator=(ThreadLocalCache &&) = delete;

	static ThreadLocalCache &Singlon()
	{
		thread_local ThreadLocalCache instance;
		return instance;
	}
	std::unordered_map<std::string, std::shared_ptr<T>> cached;
};

template <typename T>
struct Reloadable {
	Reloadable(unsigned int seconds) : interval(seconds) {}

	unsigned int interval;
	T ping, pong, *curr = &ping;
	std::atomic<time_t> last_time{0};

	inline std::shared_ptr<T> which()
	{
		return std::shared_ptr<T>(curr, [](T *) {});
	}

	inline bool reload()
	{
		if (curr == &ping) {
			curr = &pong;
			std::cout << "Switch to pong\n";
		} else {
			curr = &ping;
			std::cout << "Switch to ping\n";
		}
		return true;
	}
};

template <typename T>
class ReloadableTimerEvent : public TimerEvent {
  public:
	explicit ReloadableTimerEvent(TimerWheel &time_wheel, std::shared_ptr<Reloadable<T>> reloadable) : time_wheel_(time_wheel), reloadable_(reloadable) {}

  protected:
	void execute() override
	{
		reloadable_->reload();
		time_wheel_.schedule(this, reloadable_->interval);
	}

  private:
	TimerWheel &time_wheel_;
	std::shared_ptr<Reloadable<T>> reloadable_;
};

class ReloadableManager {
  public:
	static ReloadableManager &Singlon()
	{
		static ReloadableManager instance;
		return instance;
	}

	template <typename T>
	void AddReloadable(std::shared_ptr<Reloadable<T>> &reloadable)
	{
		TimerEvent *event = new ReloadableTimerEvent<T>(M_timer_wheel, reloadable);
		M_timer_wheel.schedule(event, reloadable->interval);
	}

	ReloadableManager()
	{
		M_worker = std::thread([&] {
			TickType seconds = 0;
			while (!stop) {
				if (seconds > 0) { M_timer_wheel.advance(seconds); }
				seconds = M_timer_wheel.ticks_to_wakeup();
				sleep(seconds);
			}
		});
	}

	~ReloadableManager()
	{
		stop = true;
		M_worker.join();
	}

  private:
	bool stop = false;
	std::thread M_worker;
	TimerWheel M_timer_wheel;
};

class SharedStorage {
  public:
	SharedStorage() = default;

	static SharedStorage &Singlon()
	{
		static SharedStorage instance;
		return instance;
	}

	SharedStorage(const SharedStorage &) = delete;
	SharedStorage &operator=(const SharedStorage &) = delete;

	SharedStorage(SharedStorage &&) = delete;
	SharedStorage &operator=(SharedStorage &&) = delete;

	template <typename T, typename... Tags>
	std::shared_ptr<T> GetOrNull(Tags &&...tags)
	{
		auto key = MakeKey<T>(std::forward<Tags>(tags)...);
		if (auto cached = ThreadLocalCache<T>::Get(key)) { return cached; }

		std::shared_lock guard(M_mutex);
		auto it = M_storage.find(key);
		if (it != M_storage.end()) {
			if (it->second.type != ConstexprGetType<T>()) { throw std::runtime_error("Type mismatch for key: [" + key + "]"); }
			return std::static_pointer_cast<T>(it->second.value);
		}
		return nullptr;
	}

	template <typename T, typename... Tags>
	std::shared_ptr<T> GetOrCreate(Tags &&...tags)
	{
		auto key = MakeKey<T>(std::forward<Tags>(tags)...);
		if (auto cached = ThreadLocalCache<T>::Get(key)) { return cached; }

		std::unique_lock guard(M_mutex);
		auto &entry = M_storage[key];
		if (!entry.value) {
			INC(shared_misses);
			entry.type = ConstexprGetType<T>();
			entry.value = std::make_shared<T>();
		} else {
			INC(shared_hits);
			if (entry.type != ConstexprGetType<T>()) { throw std::runtime_error("Type mismatch for key: [" + key + "]"); }
		}

		if (!entry.deletable) { ThreadLocalCache<T>::Set(key, std::static_pointer_cast<T>(entry.value)); }

		return std::static_pointer_cast<T>(entry.value);
	}

	template <typename T, typename... Tags>
	bool Has(Tags &&...tags)
	{
		auto key = MakeKey<T>(std::forward<Tags>(tags)...);
		if (ThreadLocalCache<T>::Has(key)) { return true; }

		std::shared_lock guard(M_mutex);
		auto it = M_storage.find(key);
		return it != M_storage.end() && it->second.type == ConstexprGetType<T>();
	}

	template <typename T, typename... Tags>
	bool Delete(Tags &&...tags)
	{
		auto key = MakeKey<T>(std::forward<Tags>(tags)...);

		std::unique_lock guard(M_mutex);
		auto it = M_storage.find(key);
		if (it != M_storage.end()) {
			if (it->second.type != ConstexprGetType<T>()) { throw std::runtime_error("Type mismatch for key: [" + key + "]"); }
			if (it->second.deletable) {
				M_storage.erase(it);
				return true;
			} else {
				throw std::runtime_error("Attempted to delete a non-deletable value for key: [" + key + "]");
			}
		}
		return false;
	}

	template <typename T, typename... Tags>
	void Deletable(Tags &&...tags)
	{
		auto key = MakeKey<T>(std::forward<Tags>(tags)...);
		std::unique_lock guard(M_mutex);
		auto it = M_storage.find(key);
		if (it != M_storage.end()) {
			if (it->second.type != ConstexprGetType<T>()) { throw std::runtime_error("Type mismatch for key: [" + key + "]"); }
			it->second.deletable = true;
		} else {
			auto &entry = M_storage[key];
			entry.type = key;
			entry.deletable = true;
		}
	}

	template <typename T, typename... Tags>
	std::shared_ptr<T> GetOrNull_Reloadable(Tags &&...tags)
	{
		using U = Reloadable<T>;
		auto key = MakeKey<U>(std::forward<Tags>(tags)...);
		if (auto cached = ThreadLocalCache<U>::Get(key)) { return cached; }

		std::shared_lock guard(M_mutex);
		auto it = M_storage.find(key);
		if (it != M_storage.end()) {
			if (it->second.type != ConstexprGetType<U>()) { throw std::runtime_error("Type mismatch for key: [" + key + "]"); }
			return std::static_pointer_cast<U>(it->second.value)->which();
		}
		return nullptr;
	}

	template <typename T, typename... Tags>
	std::shared_ptr<T> GetOrCreate_Reloadable(time_t interval, Tags &&...tags)
	{
		using U = Reloadable<T>;
		auto key = MakeKey<U>(std::forward<Tags>(tags)...);
		if (auto cached = ThreadLocalCache<U>::Get(key)) { return cached->which(); }
		++shared_hits;

		std::unique_lock guard(M_mutex);
		auto &entry = M_storage[key];
		if (!entry.value) {
			auto value = std::make_shared<U>(interval);
			entry.type = ConstexprGetType<U>();
			entry.value = value;
			entry.reloadable = true;
			ReloadableManager::Singlon().AddReloadable(value);
		} else {
			INC(shared_hits);
			if (entry.type != ConstexprGetType<U>()) { throw std::runtime_error("Type mismatch for key: [" + key + "]"); }
		}

		if (!entry.deletable) { ThreadLocalCache<U>::Set(key, std::static_pointer_cast<U>(entry.value)); }

		return std::static_pointer_cast<U>(entry.value)->which();
	}

	template <typename T, typename... Tags>
	bool Has_Reloadable(Tags &&...tags)
	{
		using U = Reloadable<T>;
		auto key = MakeKey<U>(std::forward<Tags>(tags)...);
		if (ThreadLocalCache<U>::Has(key)) { return true; }

		std::shared_lock guard(M_mutex);
		auto it = M_storage.find(key);
		return it != M_storage.end() && it->second.type == ConstexprGetType<U>();
	}

  private:
	struct Entry {
		std::string_view type;
		std::shared_ptr<void> value;
		bool deletable = false;
		bool reloadable = false;
	};
	std::shared_mutex M_mutex;
	std::unordered_map<std::string, Entry> M_storage;
};
} // namespace
