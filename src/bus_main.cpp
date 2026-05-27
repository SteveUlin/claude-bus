// Entry point for the unified `bus` binary. Argv parser + dispatcher.
// The actual subcommand implementations live under src/sub/.

#include "bus.h"

#include <cstdio>
#include <print>
#include <span>
#include <string_view>

auto main(int argc, const char* argv[]) -> int {
  if (argc < 2) {
    bus::printUsage(stderr);
    return 2;
  }
  const std::string_view cmd{argv[1]};
  const std::span<const char* const> rest{argv + 2,
                                          static_cast<std::size_t>(argc - 2)};
  for (std::size_t i = 0; i < bus::kSubcommandsCount; ++i) {
    if (bus::kSubcommands[i].name == cmd) {
      return bus::kSubcommands[i].handler(rest);
    }
  }
  std::println(stderr, "bus: unknown subcommand \"{}\"", cmd);
  bus::printUsage(stderr);
  return 2;
}
