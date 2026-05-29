#include "harness.h"

#include <print>

namespace bus::test {

auto registry() -> std::vector<std::pair<std::string, TestFn>>& {
  static std::vector<std::pair<std::string, TestFn>> r;
  return r;
}

auto failures() -> int& {
  static int f = 0;
  return f;
}

Registrar::Registrar(const char* name, TestFn fn) {
  registry().emplace_back(name, fn);
}

void recordCheck(bool ok, const std::string& msg, const char* file, int line) {
  if (ok) return;
  ++failures();
  std::println(stderr, "    FAIL {}:{}  {}", file, line, msg);
}

}  // namespace bus::test

auto main() -> int {
  using namespace bus::test;
  int failed_tests = 0;
  for (const auto& [name, fn] : registry()) {
    const int before = failures();
    fn();
    if (failures() > before) {
      ++failed_tests;
      std::println(stderr, "[FAIL] {}", name);
    } else {
      std::println("[ ok ] {}", name);
    }
  }
  if (failures() == 0) {
    std::println("\nAll {} test(s) passed.", registry().size());
    return 0;
  }
  std::println(stderr, "\n{} test(s) failed ({} check(s) failed).",
               failed_tests, failures());
  return 1;
}
