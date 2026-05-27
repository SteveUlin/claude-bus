#pragma once

// Public surface of the unified `bus` binary.
//
// Subcommand handlers have a uniform signature: argv[2..] arrives as
// `args`; the handler returns the process exit code. Each subcommand's
// implementation lives under src/sub/, and its declaration in src/sub.h.
//
// The registry (`kSubcommands`) is defined in src/bus.cpp and grows as
// each phase ports more subcommands. `printUsage` walks the registry
// for `bus help` and for the unknown-subcommand error path.

#include <cstdio>
#include <cstddef>
#include <span>
#include <string_view>

namespace bus {

using SubHandler = int (*)(std::span<const char* const>);

struct Subcommand {
  std::string_view name;
  SubHandler handler;
  std::string_view summary;
};

extern const Subcommand kSubcommands[];
extern const std::size_t kSubcommandsCount;

auto printUsage(std::FILE* out) -> void;

}  // namespace bus
