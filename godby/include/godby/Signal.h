#pragma once

#include <vector>			   // std::vector
#include <variant>			   // std::variant
#include <functional>		   // std::function
#include <unordered_map>	   // std::unordered_map
#include <godby/Spinlock.h>	   // godby::Spinlock
#include <godby/Portability.h> // Portability

namespace godby
{
class Signal {
  public:
	static int Install(int sig);

	static int Register(std::function<void(int)> handler);

	static int Register(int sig, std::function<void()> handler);

	static int Unregister(int id, const char *file = __builtin_FILE(), int line = __builtin_LINE());

  private:
	int id = 0;
	godby::Spinlock lock;
	std::unordered_map<int, std::vector<int>> handlers_by_sig;
	std::unordered_map<int, std::variant<std::function<void()>, std::function<void(int)>>> id_to_handler;

	Signal() = default;
	~Signal() = default;

	static Signal &Instance();
	static void sig_handler(int sig);

	int Add(std::function<void(int)> handler);

	int Add(int sig, std::function<void()> handler);

	int Remove(int id);

	void OnSignal(int sig);
};
} // namespace godby
