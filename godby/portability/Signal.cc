#include <functional>	  // std::function
#include <vector>		  // std::vector
#include <mutex>		  // std::unique_lock
#include <cstdio>		  // fprintf, stderr
#include <signal.h>		  // sigaction, sigemptyset
#include <godby/Signal.h> // godby::Signal

namespace godby
{
Signal &Signal::Instance()
{
	static Signal instance;
	return instance;
}

void Signal::sig_handler(int sig)
{
	Signal::Instance().OnSignal(sig);
}

int Signal::Install(int sig)
{
	struct sigaction sa;
	sa.sa_flags = 0;
	sa.sa_handler = sig_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(sig, &sa, nullptr) != 0) {
		fprintf(stderr, "sigaction failed\n");
		struct sigaction sa;
		sa.sa_flags = 0;
		sa.sa_handler = SIG_DFL;
		sigemptyset(&sa.sa_mask);
		sigaction(sig, &sa, nullptr);
		return -errno;
	}
	return 0;
}

int Signal::Register(std::function<void(int)> handler)
{
	return Instance().Add(handler);
}

int Signal::Register(int sig, std::function<void()> handler)
{
	return Instance().Add(sig, handler);
}

int Signal::Unregister(int id, const char *file, int line)
{
	printf("Unregister: %d @ %s:%d\n", id, file, line);
	return Instance().Remove(id);
}

int Signal::Add(std::function<void(int)> handler)
{
	std::unique_lock guard(lock);
	id_to_handler[++id] = handler;
	return id;
}

int Signal::Add(int sig, std::function<void()> handler)
{
	std::unique_lock guard(lock);
	id_to_handler[++id] = handler;
	handlers_by_sig[sig].push_back(id);
	return id;
}

int Signal::Remove(int id)
{
	std::unique_lock guard(lock);
	auto it = id_to_handler.find(id);
	if (it != id_to_handler.end()) {
		if (std::holds_alternative<std::function<void()>>(it->second)) {
			for (auto &[sig, handlers] : handlers_by_sig) { handlers.erase(std::remove(handlers.begin(), handlers.end(), id), handlers.end()); }
		} else {
			for (auto &[sig, handler_ids] : handlers_by_sig) { handler_ids.erase(std::remove(handler_ids.begin(), handler_ids.end(), id), handler_ids.end()); }
		}
		id_to_handler.erase(it);
	}
	return 0;
}

void Signal::OnSignal(int sig)
{
	std::unique_lock guard(lock);
	if (handlers_by_sig.find(sig) != handlers_by_sig.end()) {
		for (int id : handlers_by_sig[sig]) {
			if (std::holds_alternative<std::function<void()>>(id_to_handler[id])) { std::get<std::function<void()>>(id_to_handler[id])(); }
		}
	}
	for (const auto &[id, handler] : id_to_handler) {
		if (std::holds_alternative<std::function<void(int)>>(handler)) { std::get<std::function<void(int)>>(handler)(sig); }
	}
}
} // namespace godby
