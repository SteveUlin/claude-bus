#pragma once

// The bus state root — the single, durable definition of where all bus
// state lives. Resolution order:
//   1. $CLAUDE_BUS_STATE        — explicit override (tests, isolation).
//   2. $XDG_STATE_HOME/claude-bus
//   3. $HOME/.local/state/claude-bus
//   4. /tmp/claude-bus          — last resort (no HOME, e.g. a bare env).
//
// WHY this exists: the default used to be `/tmp/claude-bus`, duplicated
// inline at ~12 call sites. /tmp is tmpfs on most systems — a reboot
// wipes it, taking the topic logs, per-consumer cursors, the boot-epoch
// fence, accumulated learnings, and the off-TTY sentinels with it. That
// makes the "durable out-of-context system-of-record" a lie. Persisting
// across reboot is SAFE: the epoch fence already quarantines records
// from a previous broker run, so a surviving log is never re-delivered
// as new.
//
// Centralizing here also kills the drift class: one definition, every
// reader (broker, delivery loop, RPC socket, viewers) agrees. A new
// reader calls bus::stateRoot() instead of hard-coding a path.

#include <cstdlib>
#include <string>

namespace bus {

// Canonical path for a named topic log under a given state root.
// Used by both Journal (byte ops) and CursorStore (cursor ops) to
// derive the log path from (state_root, name) consistently.
inline auto topicLogPath(std::string_view state_root,
                         std::string_view name) -> std::string {
  return std::string{state_root} + "/topics/" + std::string{name} + ".log";
}

inline auto stateRoot() -> std::string {
  if (const char* s = std::getenv("CLAUDE_BUS_STATE"); s != nullptr && *s) {
    return s;
  }
  if (const char* x = std::getenv("XDG_STATE_HOME"); x != nullptr && *x) {
    return std::string{x} + "/claude-bus";
  }
  if (const char* h = std::getenv("HOME"); h != nullptr && *h) {
    return std::string{h} + "/.local/state/claude-bus";
  }
  return "/tmp/claude-bus";
}

}  // namespace bus
