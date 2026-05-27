#pragma once

// TUI dispatch state machine. Lifted from the original bin/dispatch-tui
// shell into C++ so the broker can call it in-process (no fork+exec
// per dispatch).
//
// Semantics:
//   1. Resolve agent name to pane id via paneState.
//   2. Hold the per-pane flock at $STATE/tui-locks/<agent>.lock so
//      concurrent bus producers serialize at the TTY.
//   3. Detect ready: pane-state says mode=INSERT + buffer=(empty).
//      If not ready, normalize via Esc Esc + i and retry with
//      exponential backoff (0.25 / 1 / 4 s). Up to 3 attempts.
//   4. On ready, write the body via sendToPane (write-chars + Enter).
//   5. Return success/failure to the caller. The caller decides what
//      to do on failure (broker delivery marks the record as failed
//      and lets the retry / escalation logic of phase 4e handle it).
//
// This is a SYNCHRONOUS call. A failed dispatch can take up to ~5.25s
// (0.25 + 1 + 4 backoff between three attempts). The broker runs the
// delivery tick at 250ms intervals and dispatches one record per topic
// per tick — a long-stuck dispatch only stalls deliveries to THAT
// topic, not others.

#include "broker.h"

#include <string_view>

namespace bus::dispatch {

// Returns true if the body was successfully written into the pane.
// false on persistent failure (pane vanished, all attempts saw the
// pane in a non-INSERT state, etc.).
auto dispatchTui(const BrokerConfig& cfg, std::string_view agent,
                 std::string_view body) -> bool;

}  // namespace bus::dispatch
