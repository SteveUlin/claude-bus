#pragma once

// Zellij pane helpers used in-process. Consolidates what used to be:
//   bin/pane-id         (name → terminal_N)
//   bin/send            (write-chars + send-keys Enter)
//   src/bin/pane_state.cpp  (ANSI dump → mode / buffer / suggestion / bypass)
//
// Everything that previously shelled out to those binaries now calls
// these functions directly. The corresponding `bus pane-id`,
// `bus pane-state`, `bus msg send` subcommands are thin CLI shells around
// these.

#include "pane_state.h"  // PaneState (value type) — acquisition added below.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bus {

// Resolve a pane name (zellij layout `name=`) to its `terminal_N` id.
// Returns empty string on miss or ambiguity (multiple panes share the
// title). Errors are written to stderr by the CLI shell, not by this
// function — keep this helper quiet.
auto paneId(std::string_view name) -> std::string;

// Type `text` into the pane and submit it with Enter. Pairs
// `zellij action write-chars` + `send-keys Enter` so the submit key
// can't be forgotten (the existing claude-code bug where `\n` in
// write-chars does NOT count as Enter for Ink-based TUIs).
//
// Returns true on success. This is a lower-level primitive than the
// broker's flock'd write; callers wanting single-writer semantics
// across competing producers must hold the per-pane flock themselves.
//
// **Does NOT preserve the user's draft.** If the pane has typed-but-
// unsent text, this routine concatenates the new text after it. Use
// sendToPaneSafe for the inbox-style "drop a message in" case.
auto sendToPane(std::string_view pane_id, std::string_view text) -> bool;

// Safe variant of sendToPane that preserves the human-typed draft.
//
// Steps:
//  1. Read paneState(agent_name) to learn the mode + buffer + cursor
//     context.
//  2. Refuse delivery if the pane is in a state we can't reason about:
//       - mode = "unknown" — the bottom status line isn't visible, so
//         the pane is scrolled away from the prompt OR showing tool
//         output without a status footer.
//       - mode = "LOCKED" — the TUI is in a non-input modal.
//     Returns false in both cases; callers should retry later.
//  3. If buffer non-empty, snapshot the captured draft. Suggestions
//     (ghost autocomplete) are claude's, not the user's, and are NOT
//     preserved.
//  4. Enter INSERT mode if not already there. (In INSERT, sending 'i'
//     would type a literal 'i' — we only key-press 'i' when leaving
//     NORMAL / VISUAL.)
//  5. Ctrl-U to clear the line, then write-chars + Enter for the new
//     text.
//  6. Re-type the saved draft via write-chars so the user can pick up
//     where they were.
//
// Known limitation: multi-line drafts get truncated to the first
// (prompt-containing) line — the input parser only inspects one row.
// For most claude TUI usage that's the entire draft; for paste-or-
// shift-enter multi-line input the trailing lines are lost. Worth
// fixing if it bites in practice.
auto sendToPaneSafe(std::string_view agent_name,
                    std::string_view text) -> bool;

// Send a single zellij key (e.g., "Esc", "Enter", "Ctrl c"). No
// Enter follow-up — for state-machine helpers that need raw key
// presses (normalize via Esc Esc + i, etc.). Returns true on success.
auto sendKey(std::string_view pane_id, std::string_view key) -> bool;

// PaneState (the value type) lives in pane_state.h so the Readers seam can
// consume a snapshot without seeing the acquisition API below. pane.h is the
// loop/Daemon-plane header: it adds the fetch + actuators on top of the struct.
auto paneState(std::string_view name) -> PaneState;

// Per-name TTL cache over paneState (roadmap 1.1 — the broker-wedge fix).
// paneState() forks a blocking `zellij dump-screen` on the broker's
// single delivery-loop thread; the gates call it ~2-3x per agent per
// tick, so a slow zellij stalls the whole loop. This collapses N forks
// per tick to <=1 by serving a result fresher than `ttl` from memory.
//
// Single-threaded by design (the loop owns it) — NO locking. The
// fetcher and clock are injected so the policy is unit-testable without
// zellij or real time. Use paneStateCached() at the gate call sites;
// the state RPC keeps raw paneState() for monitor freshness.
class PaneStateCache {
 public:
  using Fetcher = std::function<PaneState(std::string_view)>;
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  PaneStateCache(Fetcher fetch, Clock clock, std::chrono::milliseconds ttl)
      : fetch_{std::move(fetch)}, clock_{std::move(clock)}, ttl_{ttl} {}

  auto get(std::string_view name) -> PaneState {
    const auto t = clock_();
    if (auto it = cache_.find(std::string{name});
        it != cache_.end() && (t - it->second.at) < ttl_) {
      return it->second.state;  // fresh enough — skip the fork
    }
    PaneState s = fetch_(name);
    cache_[std::string{name}] = Entry{s, t};
    return s;
  }

 private:
  struct Entry {
    PaneState state;
    std::chrono::steady_clock::time_point at;
  };
  Fetcher fetch_;
  Clock clock_;
  std::chrono::milliseconds ttl_;
  std::unordered_map<std::string, Entry> cache_;
};

// TTL in ms from $CLAUDE_BUS_PANESTATE_TTL_MS (default 300). Read once.
auto paneStateCacheTtlMs() -> std::int64_t;

// Loop-thread-only cached paneState — backed by one process-static
// PaneStateCache (real fetcher + steady_clock). No locking. The gate
// call sites use this; the state RPC stays on raw paneState().
auto paneStateCached(std::string_view name) -> PaneState;

}  // namespace bus
