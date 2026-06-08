#pragma once

// Shared utilities for bus sub-commands (header-only).

#include <string>
#include <string_view>
#include <vector>

namespace bus {

// Split a comma-separated list, trimming surrounding whitespace (spaces and
// tabs), dropping empty tokens. Matches the splitCsv behaviour originally in
// sub_tasks.cpp; the two open-coded loops in sub_produce.cpp and
// sub_topic.cpp had the same trim/empty semantics.
inline auto splitCsv(std::string_view s) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else if (c != ' ' && c != '\t') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// Truncate a string to at most `w` display characters, replacing the last
// character with '…' when truncation occurs. Matches the truncate()
// definitions in sub_tasks.cpp and sub_verify.cpp.
inline auto truncate(std::string s, std::size_t w) -> std::string {
  if (s.size() <= w) return s;
  s.resize(w - 1);
  s += "…";
  return s;
}

}  // namespace bus
