#include "godbytest.h"
#include <godby/AtomicHashmap.h>

int main(int argc, char **argv)
{
	// AtomicHashmap
	{
		godby::AtomicHashmap<std::string, int> map(8192);

		for (int i = 0; i < 4096; ++i) { map.Set(std::to_string(i), i); }

		for (int i = 0; i < 4096; ++i) {
			auto accessor = map.Get(std::to_string(i));
			ASSERT_EQ(accessor.has(), true);
			ASSERT_EQ(accessor.value(), i);
		}

		for (int i = 0; i < 500; ++i) { map.Delete(std::to_string(i)); }

		for (int i = 0; i < 4096; ++i) {
			auto accessor = map.Get(std::to_string(i));
			ASSERT_EQ(accessor.has(), i < 500 ? false : true);
		}

		for (int i = 0; i < 500; ++i) { map.Set(std::to_string(i), 10000 + i); }

		for (int i = 0; i < 500; ++i) {
			auto accessor = map.Get(std::to_string(i));
			ASSERT_EQ(accessor.has(), true);
			ASSERT_EQ(accessor.value(), 10000 + i);
		}

		map.WalkAll([](auto key, auto value) {
			ASSERT_EQ(value.has(), true);
			if (std::stoi(key.value()) < 500) {
				ASSERT_EQ(value.value(), 10000 + std::stoi(key.value()));
			} else {
				ASSERT_EQ(value.value(), std::stoi(key.value()));
			}
		});

		for (auto &bucket : map) {
			auto key = bucket.AccessKey();
			auto value = bucket.AccessValue();
			ASSERT_EQ(key.has() && value.has(), true);
			if (std::stoi(key.value()) < 500) {
				ASSERT_EQ(value.value(), 10000 + std::stoi(key.value()));
			} else {
				ASSERT_EQ(value.value(), std::stoi(key.value()));
			}
		}

		map.Cleanup();

		{
			ankerl::nanobench::Bench().run("AtomicHashmap::Set", [&] { map.Set("5", 5); });
			ankerl::nanobench::Bench().run("AtomicHashmap::Get", [&] { map.Get("5"); });
		}
	}

	return 0;
}
