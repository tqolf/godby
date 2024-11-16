#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include <godby/SharedStorage.h>

using namespace godby;

int test_main()
{
	constexpr size_t numThreads = 8;
	constexpr size_t iterations = 1000;
	std::vector<std::thread> threads;

	auto &storage = SharedStorage::Singlon();
	for (size_t i = 0; i < numThreads; ++i) {
		threads.emplace_back([&](size_t id, size_t iterations) {
			for (size_t i = 0; i < iterations; ++i) {
				auto value = storage.GetOrCreate<size_t>("thread", id);
				++(*value);
				assert(storage.Has<size_t>("thread", id));
				// std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			auto finalValue = storage.GetOrNull<size_t>("thread", id);
			assert(finalValue && *finalValue == iterations);
			TRACE("Thread %zu completed with value: %zu", id, *finalValue);
		}, i, iterations);
	}

	for (auto &t : threads) { t.join(); }

	for (size_t i = 0; i < numThreads; ++i) {
		auto value = storage.GetOrNull<size_t>("thread", std::to_string(i));
		if (value) {
			TRACE("Final value for thread %zu: %zu", i, *value);
		} else {
			TRACE("Value for thread %zu not found!", i);
		}
	}

	TRACE("local-hit: %zu, local-misses: %zu, creats: %zu", local_hits.load(), local_misses.load(), shared_misses.load());

	return 0;
}

int main()
{
	auto &storage = SharedStorage::Singlon();

	try {
		storage.Deletable<int>("tag1", "tag2");
		auto value = storage.GetOrCreate<int>("tag1", "tag2");
		*value = 42;

		auto value2 = storage.GetOrCreate<std::string>();
		*value2 = "Hello, world!";

		if (storage.Has<int>("tag1", "tag2")) {
			auto fetchedValue = storage.GetOrNull<int>("tag1", "tag2");
			if (fetchedValue) { std::cout << "Value: " << *fetchedValue << "\n"; }
		}

		auto value3 = storage.GetOrCreate_Reloadable<std::string>(1);
		*value3 = "Hello world [Reloadable]";
		sleep(10);

		if (storage.Has<std::string>()) {
			auto fetchedValue = storage.GetOrNull<std::string>();
			if (fetchedValue) { std::cout << "Value: " << *fetchedValue << "\n"; }
		}

		if (storage.Has<std::string>("tag1")) {
			auto fetchedValue = storage.GetOrNull<std::string>();
			if (fetchedValue) { std::cout << "Value: " << *fetchedValue << "\n"; }
		} else {
			std::cout << "Key not found\n";
		}

		try {
			auto wrongType = storage.GetOrNull<double>("tag1", "tag2");
		} catch (const std::runtime_error &e) {
			std::cout << "Error: " << e.what() << "\n";
		}

		if (storage.Delete<int>("tag1", "tag2")) { std::cout << "Value deleted\n"; }
	} catch (const std::runtime_error &e) {
		std::cerr << "Runtime error: " << e.what() << "\n";
	}

	test_main();

	return 0;
}
