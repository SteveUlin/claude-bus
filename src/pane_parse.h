#pragma once

// Pure parser for zellij `dump-screen --ansi` output. String-in / struct-out,
// no syscalls — this is the logic most exposed to zellij/Claude-TUI output
// drift (mode markers, the bypass footer, the `❯` input row, ghost
// suggestions). Split out of pane.cpp's IO layer so it can be unit-tested in
// isolation (tests/unit/test_pane_parse.cpp); the pane actuator (paneState)
// is the production caller.
//
// Internal helpers (ANSI SGR walk, whitespace/divider trimming, divider-line
// detection) stay file-local in pane_parse.cpp — only the surface paneState
// needs is declared here.

#include <string>
#include <string_view>
#include <vector>

namespace bus::pane_parse {

// The human-typed buffer and Claude's ghost suggestion, separated out of the
// input row by SGR dim attribute.
struct InputParts {
  std::string buffer;
  std::string suggestion;
};

// Strip all ANSI escape sequences, yielding plain text (newlines preserved).
auto stripAnsi(std::string_view s) -> std::string;

// Split on '\n' into lines (no trailing empty line for a final newline).
auto splitLines(std::string_view s) -> std::vector<std::string>;

// Detect the Claude-TUI vim mode ("INSERT" | "NORMAL" | "VISUAL" | "LOCKED"),
// or "unknown" when the status footer isn't visible. Scans the whole screen
// and falls back to INSERT when only the "bypass permissions" footer shows.
auto detectMode(const std::vector<std::string>& lines) -> std::string;

// "on" when the "bypass permissions on" footer is visible, else "off".
auto detectBypass(const std::vector<std::string>& lines) -> std::string;

// Index of the bottom input row (the last `❯` line sitting just below a
// divider), or -1 if not found.
auto findInputLine(const std::vector<std::string>& lines) -> int;

// Extract the raw (ANSI-laced) bytes of line `target_line` from a dump.
auto extractAnsiLine(std::string_view dump, int target_line) -> std::string;

// Parse the input row's ANSI bytes into {buffer, suggestion}.
auto parseInput(std::string_view ansi_line) -> InputParts;

}  // namespace bus::pane_parse
