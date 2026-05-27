// Built-in meta subcommands: `bus help`, `bus version`.

#include "../bus.h"
#include "../sub.h"

#include <cstdio>
#include <print>
#include <span>

namespace bus {

auto subHelp(std::span<const char* const>) -> int {
  // `bus help` prints to stdout (not stderr) so `bus help | grep ...`
  // and shell completion scripts work. The error-path usage in
  // bus_main.cpp prints to stderr; same content, different stream.
  printUsage(stdout);
  return 0;
}

auto subVersion(std::span<const char* const>) -> int {
  std::println("claude-bus 0.4.0-alpha");
  return 0;
}

}  // namespace bus
