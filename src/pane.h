#pragma once

// Zellij pane helpers used in-process. Consolidates what used to be:
//   bin/pane-id         (name → terminal_N)
//   bin/send            (write-chars + send-keys Enter)
//   src/bin/pane_state.cpp  (ANSI dump → mode / buffer / suggestion / bypass)
//
// Everything that previously shelled out to those binaries now calls
// these functions directly. The corresponding `bus pane-id`,
// `bus pane-state`, `bus send` subcommands are thin CLI shells around
// these.

#include <string>
#include <string_view>

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

// State of a Claude Code TUI pane derived from
// `zellij action dump-screen --ansi`. All fields are best-effort:
// `ok=false` means the dump failed (pane gone, zellij absent, etc.);
// empty strings on individual fields are the wire-level "(empty)"
// sentinel from the pane parser.
struct PaneState {
  bool ok{false};
  std::string mode;          // INSERT / NORMAL / VISUAL / LOCKED / unknown
  std::string buffer;        // typed input; "(empty)" when blank
  std::string suggestion;    // ghost autocomplete; "(none)" when blank
  std::string bypass_perms;  // "on" / "off" / "" (unknown)
};

auto paneState(std::string_view name) -> PaneState;

}  // namespace bus
