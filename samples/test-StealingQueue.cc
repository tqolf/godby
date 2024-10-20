
#include <thread>
#include <iostream>
#include <godby/StealingQueue.h>

int main()
{
	// work-stealing queue of integer items
	godby::StealingQueue<int> queue;

	// only one thread can push and pop
	std::thread owner([&]() {
		for (int i = 0; i < 100000000; i = i + 1) { queue.push(i); }
		while (!queue.empty()) { [[maybe_unused]] auto item = queue.pop(); }
	});

	// multiple thives can steal
	std::vector<std::thread> thiefs;
	for (size_t i = 0; i < 12; ++i) {
		thiefs.emplace_back([&]() {
			while (!queue.empty()) { [[maybe_unused]] auto item = queue.steal(); }
		});
	}

	owner.join();
	for (auto &thief : thiefs) { thief.join(); }

	return 0;
}
