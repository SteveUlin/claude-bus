#pragma once

// Minimal table-test harness in the flat bus:: style — no third-party
// dependency. A TEST(name) block self-registers via a static initializer;
// `bus_tests`' main() runs every registered test and exits non-zero if any
// CHECK failed. Value-printing on failure uses std::format when the type is
// formattable (enums fall back to "<?>" — compare via axisName()/stateName()
// for readable output).

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bus::test {

using TestFn = void (*)();

auto registry() -> std::vector<std::pair<std::string, TestFn>>&;
auto failures() -> int&;
void recordCheck(bool ok, const std::string& msg, const char* file, int line);

struct Registrar {
  Registrar(const char* name, TestFn fn);
};

template <class T>
auto toStr(const T& v) -> std::string {
  if constexpr (std::formattable<const T&, char>) {
    return std::format("{}", v);
  } else {
    return "<?>";
  }
}

}  // namespace bus::test

#define TEST(name)                                                       \
  static void name();                                                    \
  static ::bus::test::Registrar registrar_##name{#name, name};           \
  static void name()

#define CHECK(cond) \
  ::bus::test::recordCheck((cond), "CHECK(" #cond ")", __FILE__, __LINE__)

// Note: the stringized exprs (#a/#b) can contain '{' (e.g.
// std::string{"x"}), so they must NEVER land in a std::format format
// string — build the message by concatenation instead.
#define CHECK_EQ(a, b)                                                   \
  do {                                                                   \
    auto _ca = (a);                                                      \
    auto _cb = (b);                                                      \
    ::bus::test::recordCheck(                                            \
        _ca == _cb,                                                      \
        std::string{"CHECK_EQ(" #a ", " #b ")  ->  "} +                  \
            ::bus::test::toStr(_ca) + " != " + ::bus::test::toStr(_cb),   \
        __FILE__, __LINE__);                                             \
  } while (0)
