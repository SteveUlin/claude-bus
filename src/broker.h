#pragma once

// claude-bus broker — single daemon process per session that owns
// the topic registry, topic logs, in-flight tracker, and the
// delivery loop. CLI tools (bus msg enqueue / bus msg fetch / bus msg peek / topic / …)
// reach it via JSON-RPC over a Unix socket; hooks reach it indirectly
// through events.jsonl (the broker tails it).
//
// Phase 4b.1 contains only the skeleton:
//   - socket binding via src/rpc.{h,cpp}
//   - singleton-guard via $STATE/broker.pid
//   - SIGTERM-clean shutdown
//   - a "ping" op for end-to-end verification
//
// Phase 4b.2+ layers on topic registry, topic logs, delivery, etc.

#include <string>

namespace bus {

struct BrokerConfig {
  std::string state_dir;     // bus::stateRoot() — $CLAUDE_BUS_STATE or
                             // the durable XDG default (see state_paths.h)
  std::string socket_path;   // $state/broker.sock
  std::string pid_path;      // $state/broker.pid
};

// Resolve config from environment + defaults.
auto resolveConfig() -> BrokerConfig;

// Daemon entry point. Returns the broker's exit code.
//   - Acquire the singleton lock (broker.pid with O_EXCL); fail if
//     another broker is already running.
//   - Bind the socket and serve until SIGTERM / SIGINT.
//   - Clean up pid file + socket on exit.
auto runBroker(const BrokerConfig& cfg) -> int;

}  // namespace bus
