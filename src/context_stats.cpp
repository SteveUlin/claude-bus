#include "context_stats.h"

#include "json_min.h"

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace bus {

namespace {

// Scan `"<key>": <int>` after `"<scope>"` — avoids json::parse because the
// statusline/status payload has float fields (cost.total_cost_usd) and our
// json_min doesn't speak floats.
auto scanIntAfter(std::string_view content, std::string_view scope_key,
                  std::string_view leaf_key) -> long long {
  auto scope = content.find(scope_key);
  if (scope == std::string_view::npos) return -1;
  auto key = content.find(leaf_key, scope);
  if (key == std::string_view::npos) return -1;
  auto i = content.find(':', key);
  if (i == std::string_view::npos) return -1;
  ++i;
  while (i < content.size() && (content[i] == ' ' || content[i] == '\t')) ++i;
  long long n = 0;
  bool seen = false;
  while (i < content.size() && content[i] >= '0' && content[i] <= '9') {
    n = n * 10 + (content[i] - '0');
    seen = true;
    ++i;
  }
  return seen ? n : -1;
}

// Light JSON string-value extractor (no escape handling) for the float-laced
// status blob json_min can't parse. The sibling of scanIntAfter; sub_monitor
// keeps its own copy for the lane columns — a future `json_scan` util should
// unify the pair (see docs/file-decomposition.md M5 note).
auto scanStrField(std::string_view line, std::string_view key) -> std::string {
  std::string pat;
  pat.reserve(key.size() + 2);
  pat += '"';
  pat += key;
  pat += '"';
  const auto kpos = line.find(pat);
  if (kpos == std::string_view::npos) return {};
  auto i = kpos + pat.size();
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] != ':') return {};
  ++i;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] != '"') return {};
  const auto start = i + 1;
  const auto end = line.find('"', start);
  if (end == std::string_view::npos) return {};
  return std::string(line.substr(start, end - start));
}

}  // namespace

auto contextStatsFor(std::string_view state_root, std::string_view agent)
    -> CtxStats {
  CtxStats out;

  // Preferred: the statusline capture (flat JSON we control).
  {
    const std::string sl =
        std::string{state_root} + "/statusline/" + std::string{agent} + ".json";
    std::ifstream in{sl};
    if (in) {
      std::string content((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
      if (const auto v = json::parse(content); v && v->isObject()) {
        out.tokens = v->getOrInt("total_input_tokens", -1);
        out.size_tokens = v->getOrInt("context_window_size", -1);
        const auto p = v->getOrInt("used_percentage", -1);
        if (p >= 0) out.pct = static_cast<int>(p > 100 ? 100 : p);
        out.model = v->getOrString("model_id");
        out.effort = v->getOrString("effort_level");
        if (out.size_tokens > 0) return out;  // authoritative window
      }
    }
  }

  // Fallback: broker status (knob-denominated window, no effort).
  const std::string path =
      std::string{state_root} + "/status/" + std::string{agent} + ".json";
  std::ifstream in{path};
  if (!in) return out;
  std::ostringstream buf;
  buf << in.rdbuf();
  const auto content = buf.str();
  const auto pct =
      scanIntAfter(content, "\"context_window\"", "\"used_percentage\"");
  if (pct >= 0) out.pct = static_cast<int>(pct > 100 ? 100 : pct);
  out.size_tokens =
      scanIntAfter(content, "\"context_window\"", "\"context_window_size\"");
  out.model = scanStrField(content, "model");
  return out;
}

}  // namespace bus
