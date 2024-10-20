#include <csignal>
#include "godbytest.h"
#include "contrib/function2.hpp"
#include "contrib/BS_thread_pool.hpp"
#include <godby/StealingExecutor.h>

template <typename Clock = std::chrono::high_resolution_clock>
inline uint64_t Milliseconds()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

template <typename... Signatures>
using my_function = fu2::function_base<false, true, fu2::capacity_none, //
									   false, false, Signatures...>;

class MyExecutor1 : public godby::StealingExecutor<int> {
  public:
	MyExecutor1() : SUPER(1024)
	{
		Spawn(std::thread::hardware_concurrency());
	}

  protected:
	virtual void Consume(value_type &&) override {}
};

class MyExecutor2 : public godby::StealingExecutor<int, godby::StealingPolicy<true>> {
  public:
	MyExecutor2() : SUPER(1024)
	{
		Spawn(std::thread::hardware_concurrency());
	}

  protected:
	virtual void Consume(value_type &&) override {}
};

class MyExecutor3 : public godby::StealingExecutor<my_function<void()>> {
  public:
	MyExecutor3() : SUPER(1023)
	{
		Spawn(std::thread::hardware_concurrency());
	}

  protected:
	virtual void Consume(value_type &&) override {}
};

class MyExecutor4 : public godby::StealingExecutor<my_function<void()>, godby::StealingPolicy<true>> {
  public:
	MyExecutor4() : SUPER(1023)
	{
		Spawn(std::thread::hardware_concurrency());
	}

  protected:
	virtual void Consume(value_type &&) override {}
};

class MyExecutor5 : public godby::StealingExecutor<int, godby::StealingPolicy<true>> {
  public:
	MyExecutor5() : SUPER(1023)
	{
		Spawn(std::thread::hardware_concurrency());
	}

  protected:
	virtual void Consume(value_type &&value) override
	{
		printf("Task %d done\n", value);
	}
};

class SimulateTaskExecutor : public godby::StealingExecutor<int> {
  public:
	SimulateTaskExecutor() : SUPER(1024)
	{
		Spawn(4);
	}

  protected:
	virtual void Consume(value_type &&value) override
	{
		std::this_thread::sleep_for(std::chrono::nanoseconds(value));
		printf("Task completed in %d ns\n", value);
	}
};

int main()
{
	{
		auto id1 = godby::Signal::Register(SIGINT, []() { printf("SIGINT\n"); });
		// auto id2 = godby::Signal::Register([](int sig) { printf("sig=%d\n", sig); });
		// godby::Signal::Unregister(id1);

		{
			SimulateTaskExecutor executor;
			executor.Submit(1);
			executor.Submit(1000);
			executor.Submit(1000);
			executor.Submit(1000000);
			executor.Submit(100000000);
			executor.Submit(500000000);
			std::this_thread::sleep_for(std::chrono::seconds(2));

			executor.WaitAll();

			printf("End Simulation\n\n");
		}

		{
			SimulateTaskExecutor executor;
			SimulateTaskExecutor::Controller::Cancellable();
			for (int i = 0; i < 100000; ++i) { executor.Submit(100 * 1000 * 1000); }

			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			std::raise(SIGINT);

			executor.WaitAll();

			printf("End Simulation With Cancellation\n\n");
		}
	}

	{
		size_t N = 1000000;

		{
			std::atomic<size_t> cnt{0};
			auto ts = Milliseconds();
			for (size_t i = 0; i < N; ++i) { ++cnt; }
			auto te = Milliseconds();
			printf("AtomicAdd: %llu\n", te - ts);
		}

		{
			MyExecutor1 executor;
			auto ts = Milliseconds();
			for (size_t i = 0; i < N; ++i) { executor.Submit(i); }
			executor.WaitAll();
			auto te = Milliseconds();
			printf("StealingPool<int>: %llu\n", te - ts);
		}

		{
			MyExecutor2 executor;
			auto ts = Milliseconds();
			for (size_t i = 0; i < N; ++i) { executor.Submit(i); }
			executor.WaitAll();
			auto te = Milliseconds();
			printf("StealingPool<int>/Waitable: %llu\n", te - ts);
		}

		{
			MyExecutor3 executor;
			auto ts = Milliseconds();
			for (size_t i = 0; i < N; ++i) {
				executor.Submit([]() {});
			}
			executor.WaitAll();
			auto te = Milliseconds();
			printf("StealingPool<functional>: %llu\n", te - ts);
		}

		{
			MyExecutor4 executor;
			auto ts = Milliseconds();
			for (size_t i = 0; i < N; ++i) {
				executor.Submit([]() {});
			}
			executor.WaitAll();
			auto te = Milliseconds();
			printf("StealingPool<functional>/Waitable: %llu\n", te - ts);
		}

		{
			BS::thread_pool executor(std::thread::hardware_concurrency());
			auto ts = Milliseconds();
			for (size_t i = 0; i < N; ++i) {
				executor.detach_task([]() {});
			}
			executor.wait();
			auto te = Milliseconds();
			printf("BS::thread_pool: %llu\n", te - ts);
		}

		printf("End Profiling\n\n");
	}

	if (0) {
		{
			MyExecutor5 executor;
			for (int i = 0; i < 20; ++i) {
				godby::WaitGroup wg;
				executor.Submit(wg, i);
				wg.wait();
			}
			printf("END\n\n");
			executor.WaitAll();
		}

		{
			MyExecutor5 executor;
			godby::WaitGroup wg;
			for (int i = 0; i < 20; ++i) { executor.Submit(wg, i); }
			wg.wait();
			printf("END\n\n");
			executor.WaitAll();
		}
	}

	printf("END main\n\n");

	return 0;
}
