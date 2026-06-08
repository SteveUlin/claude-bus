#include "pane_parse.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace bus::pane_parse {

namespace {

constexpr std::string_view kPromptMarker = "\xe2\x9d\xaf";  // U+276F ❯
constexpr std::string_view kDivider = "\xe2\x94\x80";       // U+2500 ─

// Trim leading and trailing whitespace, including UTF-8 NBSP (Claude
// Code uses U+00A0 to pad the input area; without this rule an "empty"
// input reads as a stray non-empty byte sequence).
auto trim(std::string s) -> std::string {
  auto is_ws = [&](std::size_t i) -> std::size_t {
    if (i >= s.size()) return 0;
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return 1;
    if (c == 0xc2 && i + 1 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0xa0) {
      return 2;  // UTF-8 NBSP
    }
    return 0;
  };
  std::size_t b = 0;
  while (auto n = is_ws(b)) b += n;
  std::size_t e = b;
  std::size_t last = b;
  while (e < s.size()) {
    if (auto n = is_ws(e)) {
      e += n;
    } else {
      ++e;
      last = e;
    }
  }
  return s.substr(b, last - b);
}

// Cut at the first box-drawing horizontal (U+2500). The wrapping row
// can land the trailing divider on the same physical line as the `❯`
// prompt; those bytes are decoration, not buffer content.
auto cutAtDivider(std::string s) -> std::string {
  const auto pos = s.find(kDivider);
  if (pos != std::string::npos) s.resize(pos);
  return s;
}

// Walk an ANSI-laced byte stream, calling `emit(text, dim)` for each
// run of printable bytes between escape sequences. `dim` is set when
// the most recent SGR for that run includes the dim attribute (`2`) or
// a foreground color whose luminance is in the dark-grey range Claude
// Code uses for ghost suggestions.
template <typename Emit>
auto walkAnsi(std::string_view s, Emit emit) -> void {
  bool dim_attr = false;
  bool dim_color = false;
  std::string run;

  auto flush = [&] {
    if (run.empty()) return;
    emit(std::string_view{run}, dim_attr || dim_color);
    run.clear();
  };

  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
      flush();
      std::size_t j = i + 2;
      while (j < s.size() && !(s[j] >= 0x40 && s[j] <= 0x7e)) ++j;
      if (j >= s.size()) break;
      const char final_byte = s[j];
      const std::string_view params{s.data() + i + 2, j - (i + 2)};
      i = j + 1;
      if (final_byte != 'm') continue;

      std::vector<int> nums;
      {
        int cur = 0;
        bool have = false;
        for (char c : params) {
          if (c == ';' || c == ':') {
            nums.push_back(have ? cur : 0);
            cur = 0;
            have = false;
          } else if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            have = true;
          }
        }
        nums.push_back(have ? cur : 0);
      }

      for (std::size_t k = 0; k < nums.size(); ++k) {
        const int n = nums[k];
        if (n == 0) {
          dim_attr = false;
          dim_color = false;
        } else if (n == 2) {
          dim_attr = true;
        } else if (n == 22) {
          dim_attr = false;
        } else if (n == 39) {
          dim_color = false;
        } else if (n == 38 && k + 1 < nums.size()) {
          const int mode = nums[k + 1];
          if (mode == 5 && k + 2 < nums.size()) {
            const int idx = nums[k + 2];
            // xterm 256 palette: 232..255 is a 24-step greyscale ramp.
            // Treat the bottom third as "ghost text".
            dim_color = idx >= 232 && idx <= 243;
            k += 2;
          } else if (mode == 2 && k + 4 < nums.size()) {
            const int r = nums[k + 2];
            const int g = nums[k + 3];
            const int b = nums[k + 4];
            // Dark-grey heuristic: all channels low and roughly equal.
            const int hi = std::max({r, g, b});
            const int lo = std::min({r, g, b});
            dim_color = hi <= 120 && (hi - lo) <= 20;
            k += 4;
          }
        }
      }
      continue;
    }
    if (s[i] == '\n') {
      flush();
      emit(std::string_view{"\n"}, false);
      ++i;
      continue;
    }
    run.push_back(s[i++]);
  }
  flush();
}

auto isDividerLine(std::string_view line) -> bool {
  std::size_t hits = 0;
  for (std::size_t i = 0; i + kDivider.size() <= line.size(); ++i) {
    if (line.substr(i, kDivider.size()) == kDivider) ++hits;
  }
  return hits >= 10;
}

}  // namespace

auto stripAnsi(std::string_view s) -> std::string {
  std::string out;
  out.reserve(s.size());
  walkAnsi(s, [&](std::string_view chunk, bool) { out.append(chunk); });
  return out;
}

auto splitLines(std::string_view s) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      lines.emplace_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < s.size()) lines.emplace_back(s.substr(start));
  return lines;
}

// Find the bottom input area: the last line containing `❯` AND sitting
// between divider lines. Returns line index or -1 if not found.
auto findInputLine(const std::vector<std::string>& lines) -> int {
  for (int i = static_cast<int>(lines.size()) - 1; i >= 1; --i) {
    if (lines[i].find(kPromptMarker) == std::string::npos) continue;
    if (i > 0 && isDividerLine(lines[i - 1])) return i;
  }
  return -1;
}

auto detectMode(const std::vector<std::string>& lines) -> std::string {
  // Scan the whole screen for `-- WORD --` with one of the known modes.
  // Claude often renders the mode marker on row N while padding rows
  // N+1..end with blanks, so a fixed bottom-N window misses it and
  // returns "unknown" — which then falsely trips BootStuck downstream.
  // The needle is specific to the claude footer.
  constexpr std::array<std::string_view, 4> kModes{"INSERT", "NORMAL", "VISUAL",
                                                   "LOCKED"};
  for (const auto& l : lines) {
    for (auto mode : kModes) {
      std::string needle = "-- ";
      needle.append(mode);
      needle.append(" --");
      if (l.find(needle) != std::string::npos) return std::string{mode};
    }
  }
  // Fallback: claude TUI sometimes hides the explicit `-- INSERT --`
  // marker in the bottom status row (e.g., when the row displays the
  // `← for agents` hint instead). In that case we still observe the
  // canonical "bypass permissions" footer string, which means the
  // status footer IS visible — we just can't see the mode marker.
  // Treat that as INSERT: the dominant claude-TUI state, and the
  // downstream sendToPaneSafe still issues `i` + Ctrl-U before any
  // write so a false-positive (rare NORMAL/VISUAL with marker hidden)
  // self-corrects. Without this fallback, every delivery to a pane
  // showing the alt-status variant defers forever — that was the
  // bast-wedge symptom 2026-05-28.
  for (const auto& l : lines) {
    if (l.find("bypass permissions") != std::string::npos) return "INSERT";
  }
  return "unknown";
}

auto detectBypass(const std::vector<std::string>& lines) -> std::string {
  // The string is unique to claude's status footer. A tall pane can push
  // the footer out of any fixed bottom-N window (long tool output, an
  // active permission prompt overlay, trailing blank rows), so scan the
  // whole visible screen — dump-screen is bounded to pane height, no
  // scrollback, so this stays cheap.
  for (const auto& l : lines) {
    if (l.find("bypass permissions on") != std::string::npos) return "on";
  }
  return "off";
}

auto extractAnsiLine(std::string_view dump, int target_line) -> std::string {
  int line = 0;
  std::size_t start = 0;
  std::size_t i = 0;
  while (i < dump.size() && line < target_line) {
    if (dump[i] == '\n') {
      ++line;
      start = i + 1;
    }
    ++i;
  }
  std::size_t end = start;
  while (end < dump.size() && dump[end] != '\n') ++end;
  return std::string{dump.substr(start, end - start)};
}

auto parseInput(std::string_view ansi_line) -> InputParts {
  InputParts parts;
  bool past_prompt = false;
  std::string pending;
  walkAnsi(ansi_line, [&](std::string_view chunk, bool dim) {
    if (chunk == "\n") return;
    if (!past_prompt) {
      pending.append(chunk);
      const auto pos = pending.find(kPromptMarker);
      if (pos == std::string::npos) return;
      past_prompt = true;
      std::size_t rest = pos + kPromptMarker.size();
      while (rest < pending.size() && pending[rest] == ' ') ++rest;
      if (rest < pending.size()) parts.buffer.append(pending.substr(rest));
      return;
    }
    if (dim) {
      parts.suggestion.append(chunk);
    } else {
      parts.buffer.append(chunk);
    }
  });
  parts.buffer = trim(cutAtDivider(std::move(parts.buffer)));
  parts.suggestion = trim(cutAtDivider(std::move(parts.suggestion)));

  // Cursor-on-suggestion fix: when Claude Code shows a ghost
  // suggestion, the cursor sits ON the first character, inverting
  // colors. That one char renders bright while the rest stays dim.
  // Our classifier then splits "add per-record CRC-32C" into
  // buffer="a" + suggestion="dd per-record CRC-32C". Re-merge when
  // buffer is exactly one character AND a suggestion follows.
  if (parts.buffer.size() == 1 && !parts.suggestion.empty()) {
    parts.suggestion = parts.buffer + parts.suggestion;
    parts.buffer.clear();
  }
  return parts;
}

}  // namespace bus::pane_parse
