#include <string>
#include <vector>
#include <rfl.hpp>
#include <rfl/yaml.hpp>
#include <rfl/internal/num_fields.hpp>

struct EmptyBase1 {};
struct EmptyBase2 {};
struct EmptyBase3 {};
struct Derived1 : public EmptyBase1 {
  int x;
  int y;
};
struct Derived2 : public EmptyBase1, public EmptyBase2 {
  int x;
  int y;
  int z;
};

struct BaseX {
  int x;
  int y;
};
struct EmptyDerived0 : BaseX, EmptyBase1 {};
struct EmptyDerived1 : EmptyBase1, BaseX {};
struct EmptyDerived2 : EmptyBase1, EmptyBase2, BaseX {};

struct BaseConfig {
    bool enable;
    std::string name;
};

struct DeriveConfig {
    bool enable;
    std::string name;
    std::string description;
    std::vector<std::string> tags;
};

struct BaseConfig1 {
    bool enable;
    std::string name;
};

struct DerivedConfig1 : public EmptyBase1, public EmptyBase2, public EmptyBase3 {
    std::string description;
    std::vector<std::string> tags;
};

#define CHECK_VIEW_SIZE(TYPE, sz) do { \
    TYPE val; \
    const auto val_view = rfl::to_view(val); \
    static_assert(val_view.size() == sz); \
} while (0)

int main()
{
    CHECK_VIEW_SIZE(Derived1, 2);
    CHECK_VIEW_SIZE(Derived2, 3);
    CHECK_VIEW_SIZE(EmptyDerived0, 2);
    CHECK_VIEW_SIZE(EmptyDerived1, 2);
    CHECK_VIEW_SIZE(EmptyDerived2, 2);

    CHECK_VIEW_SIZE(BaseConfig, 2);
    CHECK_VIEW_SIZE(DeriveConfig, 4);
    // CHECK_VIEW_SIZE(EmptyDerivedConfig1, 4);

    DerivedConfig1 config;
    // config.enable = true;
    // config.name = "hello";
    config.description = "hello world";
    config.tags = {"hello", "world"};

    // std::cout << rfl::yaml::write(config) << std::endl;

    DerivedConfig1 val;
    const auto val_view = rfl::to_view(val);
}
