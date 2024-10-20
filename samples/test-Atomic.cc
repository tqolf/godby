#include "godbytest.h"
#include <godby/Atomic.h>
#include <godby/AtomicOptional.h>
#include <godby/AtomicSharedPointer.h>
#include <godby/Spinlock.h>

static void show_statistics(std::vector<double> &all_times)
{
	std::sort(all_times.begin(), all_times.end());

	auto compute_percentile = [](const std::vector<double> &times, double percentile) {
		size_t index = static_cast<size_t>(percentile * times.size());
		return times[std::min(index, times.size() - 1)];
	};

	double p1 = compute_percentile(all_times, 0.01);
	double p50 = compute_percentile(all_times, 0.50);
	double p99 = compute_percentile(all_times, 0.99);
	double p99_95 = compute_percentile(all_times, 0.9995);

	std::cout << "    1% percentile: " << p1 << " seconds\n";
	std::cout << "   50% percentile: " << p50 << " seconds\n";
	std::cout << "   99% percentile: " << p99 << " seconds\n";
	std::cout << "99.95% percentile: " << p99_95 << " seconds\n\n";
}

template <typename Mutex, template <typename> typename Guard>
void bench_lock(int n_threads, int num_iterations)
{
	std::vector<double> all_times;

	Mutex mutex;
	std::atomic<bool> stop{false};
	std::vector<std::thread> enemies;
	enemies.reserve(n_threads - 1);
	for (int i = 0; i < n_threads - 1; i++) {
		enemies.emplace_back([&mutex, &stop]() {
			while (!stop.load(std::memory_order_relaxed)) {
				Guard<Mutex> guard(mutex);
				ankerl::nanobench::doNotOptimizeAway(guard);
			}
		});
	}

	for (int i = 0; i < num_iterations; i++) {
		auto start = std::chrono::high_resolution_clock::now();
		{
			Guard<Mutex> guard(mutex);
			ankerl::nanobench::doNotOptimizeAway(guard);
		}
		auto finish = std::chrono::high_resolution_clock::now();
		auto elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(finish - start);
		all_times.push_back(elapsed_time.count());
	}

	stop.store(true);
	for (auto &t : enemies) { t.join(); }

	show_statistics(all_times);
}

template <template <typename> typename AtomicSharedPtr, template <typename> typename SharedPtr>
void bench_load(int n_threads, int num_iterations)
{
	godby::enable_deamortized_reclamation();

	AtomicSharedPtr<int> src;
	src.store(SharedPtr<int>(new int(42)));

	std::atomic<bool> stop{false};
	std::vector<std::thread> enemies;
	enemies.reserve(n_threads - 1);
	for (int i = 0; i < n_threads - 1; i++) {
		enemies.emplace_back([mine = SharedPtr<int>(new int(i + 1)), &src, &stop]() {
			while (!stop.load(std::memory_order_relaxed)) { src.store(mine); }
		});
	}

	std::vector<double> all_times;
	for (int i = 0; i < num_iterations; i++) {
		auto start = std::chrono::high_resolution_clock::now();
		auto result = src.load();
		auto finish = std::chrono::high_resolution_clock::now();

		auto elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(finish - start);
		all_times.push_back(elapsed_time.count());
	}

	stop.store(true);
	for (auto &t : enemies) { t.join(); }

	show_statistics(all_times);
}

template <template <typename> typename AtomicSharedPtr, template <typename> typename SharedPtr>
void bench_store_delete(int n_threads, int num_iterations)
{
	AtomicSharedPtr<int> src;
	src.store(SharedPtr<int>(new int(42)));

	std::atomic<bool> stop{false};
	std::vector<std::thread> enemies;
	enemies.reserve(n_threads - 1);
	for (int i = 0; i < n_threads - 1; i++) {
		enemies.emplace_back([mine = SharedPtr<int>(new int(i + 1)), &src, &stop]() {
			while (!stop.load(std::memory_order_relaxed)) { src.store(mine); }
		});
	}

	std::vector<double> all_times;

	for (int i = 0; i < num_iterations; i++) {
		auto new_sp = SharedPtr<int>(new int(rand()));
		auto start = std::chrono::high_resolution_clock::now();
		src.store(std::move(new_sp));
		auto finish = std::chrono::high_resolution_clock::now();

		auto elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(finish - start);
		all_times.push_back(elapsed_time.count());
	}

	stop.store(true);
	for (auto &t : enemies) { t.join(); }

	show_statistics(all_times);
}

template <template <typename> typename AtomicSharedPtr, template <typename> typename SharedPtr>
void bench_store_copy(int n_threads, int num_iterations)
{
	AtomicSharedPtr<int> src;
	src.store(SharedPtr<int>(new int(42)));

	auto my_sp = SharedPtr<int>(new int(42));

	std::atomic<bool> stop{false};
	std::vector<std::thread> enemies;
	enemies.reserve(n_threads - 1);
	for (int i = 0; i < n_threads - 1; i++) {
		enemies.emplace_back([mine = SharedPtr<int>(new int(i + 1)), &src, &stop]() {
			while (!stop.load(std::memory_order_relaxed)) { src.store(mine); }
		});
	}

	std::vector<double> all_times;

	for (int i = 0; i < num_iterations; i++) {
		auto new_sp = my_sp;
		auto start = std::chrono::high_resolution_clock::now();
		src.store(std::move(new_sp));
		auto finish = std::chrono::high_resolution_clock::now();

		auto elapsed_time = std::chrono::duration_cast<std::chrono::duration<double>>(finish - start);
		all_times.push_back(elapsed_time.count());
	}

	stop.store(true);
	for (auto &t : enemies) { t.join(); }

	show_statistics(all_times);
}

int main(int argc, char **argv)
{
	// Atomic
	{
		godby::Atomic<int> ia;
		ia.store(5);
		ASSERT_EQ(ia.load(), 5);

		godby::Atomic<std::string> sa;
		sa.store("hello");
		ASSERT_EQ(sa.load(), "hello");
	}

	// RelaxedAtomic
	{
		godby::RelaxedAtomic<int> ia;
		++ia;

		ASSERT_EQ(ia.load(), 1);

		ia |= 2;
		ASSERT_EQ(ia.load(), 3);
	}

	// AtomicOptional
	{
		godby::AtomicOptional<int> ia;
		ia = 5;
		ASSERT_EQ(ia.has(), true);
		ASSERT_EQ(ia.value(), 5);

		ia = std::nullopt;
		ASSERT_EQ(ia.has(), false);

		godby::AtomicOptional<std::string> sa;
		sa = "hello";
		ASSERT_EQ(sa.has(), true);
		ASSERT_EQ(sa.value(), "hello");
	}

	// AtomicSharedPtr
	{
		godby::AtomicSharedPtr<int> p;
		godby::SharedPtr<int> s{new int(5)};
		ASSERT_EQ(s.use_count(), 1);
		p.store(s);
		ASSERT_EQ(s.use_count(), 2);

		auto s2 = p.load();
		ASSERT_EQ(s2.use_count(), 3);
		ASSERT_EQ(*s2, 5);

		bench_lock<std::shared_mutex, std::shared_lock>(8, 100000);
		bench_lock<std::shared_mutex, std::unique_lock>(8, 100000);

		bench_lock<std::mutex, std::lock_guard>(8, 100000);

		bench_lock<godby::Spinlock, std::lock_guard>(8, 100000);

		bench_load<godby::AtomicSharedPtr, godby::SharedPtr>(8, 100000);
		bench_store_delete<godby::AtomicSharedPtr, godby::SharedPtr>(8, 100000);
		bench_store_copy<godby::AtomicSharedPtr, godby::SharedPtr>(8, 100000);

		{
			godby::Atomic<int> ia;
			ankerl::nanobench::Bench().run("Atomic::load", [&] { ankerl::nanobench::doNotOptimizeAway(ia.load()); });
			ankerl::nanobench::Bench().run("Atomic::store", [&] { ia.store(5); });
		}

		{
			godby::RelaxedAtomic<int> ia;
			ankerl::nanobench::Bench().run("RelaxedAtomic::load", [&] { ankerl::nanobench::doNotOptimizeAway(ia.load()); });
			ankerl::nanobench::Bench().run("RelaxedAtomic::store", [&] { ia.store(5); });
		}

		{
			int ia;
			std::shared_mutex m;
			ankerl::nanobench::Bench().run("shared_mutex::load", [&] {
				std::shared_lock l(m);
				ankerl::nanobench::doNotOptimizeAway(ia);
			});

			ankerl::nanobench::Bench().run("shared_mutex::store", [&] {
				std::unique_lock l(m);
				ia = 5;
			});
		}
	}

	return 0;
}
