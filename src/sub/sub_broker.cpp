// `bus broker run` — broker daemon foreground entrypoint.
//
// Run is the only verb. The broker is meant to live in a foreground
// pane (typically the ops tab's floating pane); closing the pane ends
// the bus. No daemonization, no PID tracking, no stop/status RPC.
// `runBroker` itself holds a flock on broker.pid so two `bus broker
// run` invocations can't both serve at once — the second exits 1.

#include "../broker.h"
#include "../sub.h"

#include <cstdio>
#include <print>
#include <span>
#include <string_view>

namespace bus {

auto subBroker(std::span<const char* const> args) -> int {
  if (args.size() != 1 || std::string_view{args[0]} != "run") {
    std::println(stderr, "usage: bus broker run");
    return 2;
  }
  return runBroker(resolveConfig());
}

}  // namespace bus
