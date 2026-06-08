#include "harness.h"
#include "pane_parse.h"

#include <string>
#include <vector>

// pane_parse is the pure zellij-dump parser extracted from pane.cpp's IO layer.
// Before M4 it was anonymous-namespace inside the actuator TU and could not be
// reached from a test — exactly the logic most exposed to TUI output drift.

using namespace bus::pane_parse;

namespace {
// UTF-8 byte sequences the parser keys on (kept private in pane_parse.cpp).
const std::string kPrompt = "\xe2\x9d\xaf";  // U+276F ❯
const std::string kDiv = "\xe2\x94\x80";     // U+2500 ─

auto dividerLine() -> std::string {
  std::string s;
  for (int i = 0; i < 12; ++i) s += kDiv;  // isDividerLine needs >= 10 hits
  return s;
}
}  // namespace

TEST(pane_parse_stripAnsi_removes_escapes_keeps_text) {
  CHECK_EQ(stripAnsi("\x1b[31mred\x1b[0m text"), std::string{"red text"});
  // Newlines survive the strip.
  CHECK_EQ(stripAnsi("a\nb"), std::string{"a\nb"});
}

TEST(pane_parse_splitLines_no_trailing_empty) {
  const auto v = splitLines("a\nbb\nccc");
  CHECK_EQ(v.size(), 3u);
  CHECK_EQ(v[0], std::string{"a"});
  CHECK_EQ(v[1], std::string{"bb"});
  CHECK_EQ(v[2], std::string{"ccc"});
  // A trailing newline does NOT yield a final empty line.
  CHECK_EQ(splitLines("a\n").size(), 1u);
}

TEST(pane_parse_detectMode_matches_marker) {
  CHECK_EQ(detectMode({"foo", "-- INSERT --", "bar"}), std::string{"INSERT"});
  CHECK_EQ(detectMode({"-- NORMAL --"}), std::string{"NORMAL"});
  CHECK_EQ(detectMode({"-- VISUAL --"}), std::string{"VISUAL"});
  CHECK_EQ(detectMode({"-- LOCKED --"}), std::string{"LOCKED"});
}

TEST(pane_parse_detectMode_bypass_fallback_is_insert) {
  // No explicit marker, but the bypass footer is visible → INSERT fallback.
  CHECK_EQ(detectMode({"some output", "bypass permissions on"}),
           std::string{"INSERT"});
}

TEST(pane_parse_detectMode_unknown_when_footer_absent) {
  CHECK_EQ(detectMode({"just", "history", "scrolled"}),
           std::string{"unknown"});
}

TEST(pane_parse_detectBypass) {
  CHECK_EQ(detectBypass({"x", "bypass permissions on", "y"}),
           std::string{"on"});
  CHECK_EQ(detectBypass({"x", "y"}), std::string{"off"});
}

TEST(pane_parse_findInputLine_divider_then_prompt) {
  const std::vector<std::string> lines{"header", dividerLine(),
                                        kPrompt + " hello"};
  CHECK_EQ(findInputLine(lines), 2);
  // No divider above the prompt → not the input row.
  CHECK_EQ(findInputLine({"header", kPrompt + " hello"}), -1);
}

TEST(pane_parse_parseInput_plain_buffer) {
  const auto p = parseInput(kPrompt + " hello world");
  CHECK_EQ(p.buffer, std::string{"hello world"});
  CHECK_EQ(p.suggestion, std::string{""});
}

TEST(pane_parse_parseInput_dim_suggestion_split) {
  // Bright "real" is the typed buffer; dim run is Claude's ghost suggestion.
  const auto p = parseInput(kPrompt + " real\x1b[2mghost\x1b[0m");
  CHECK_EQ(p.buffer, std::string{"real"});
  CHECK_EQ(p.suggestion, std::string{"ghost"});
}

TEST(pane_parse_parseInput_cursor_on_suggestion_remerge) {
  // Cursor on the first ghost char renders it bright → buffer="a". The
  // re-merge rule folds it back into the suggestion, leaving buffer empty.
  const auto p = parseInput(kPrompt + " a\x1b[2mdd per-record\x1b[0m");
  CHECK_EQ(p.buffer, std::string{""});
  CHECK_EQ(p.suggestion, std::string{"add per-record"});
}
