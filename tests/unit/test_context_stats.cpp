#include "harness.h"
#include "context_stats.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace bus;

namespace {
auto makeRoot(const char* prefix) -> std::string {
  static int n = 0;
  const std::string sr = std::string{prefix} + std::to_string(++n);
  std::filesystem::remove_all(sr);
  return sr;
}
auto writeFile(const std::string& path, std::string_view body) -> void {
  std::filesystem::create_directories(
      std::filesystem::path{path}.parent_path());
  std::ofstream out{path};
  out << body;
}
}  // namespace

// Statusline capture is authoritative: real per-model window + live effort.
// size_tokens > 0 short-circuits the broker fallback.
TEST(context_stats_statusline_is_authoritative) {
  const auto sr = makeRoot("/tmp/claude-bus-test-ctx-sl-");
  writeFile(sr + "/statusline/alice.json",
            R"({"total_input_tokens": 50000, "context_window_size": 1000000,)"
            R"( "used_percentage": 5, "model_id": "claude-opus-4-8",)"
            R"( "effort_level": "high"})");
  const auto s = contextStatsFor(sr, "alice");
  CHECK_EQ(s.tokens, 50000LL);
  CHECK_EQ(s.size_tokens, 1000000LL);
  CHECK_EQ(s.pct, 5);
  CHECK_EQ(s.model, std::string{"claude-opus-4-8"});
  CHECK_EQ(s.effort, std::string{"high"});
  std::filesystem::remove_all(sr);
}

// used_percentage is clamped to 100.
TEST(context_stats_pct_clamped_to_100) {
  const auto sr = makeRoot("/tmp/claude-bus-test-ctx-clamp-");
  writeFile(sr + "/statusline/a.json",
            R"({"context_window_size": 200000, "used_percentage": 137})");
  CHECK_EQ(contextStatsFor(sr, "a").pct, 100);
  std::filesystem::remove_all(sr);
}

// No statusline capture → fall back to the broker status blob. That blob
// carries float fields (cost), so the reader must string-scan, not json::parse.
TEST(context_stats_falls_back_to_status_blob) {
  const auto sr = makeRoot("/tmp/claude-bus-test-ctx-fb-");
  writeFile(sr + "/status/bob.json",
            R"({"cost": {"total_cost_usd": 1.23}, "model": "claude-sonnet-4-6",)"
            R"( "context_window": {"used_percentage": 94,)"
            R"( "context_window_size": 200000}})");
  const auto s = contextStatsFor(sr, "bob");
  CHECK_EQ(s.pct, 94);
  CHECK_EQ(s.size_tokens, 200000LL);
  CHECK_EQ(s.model, std::string{"claude-sonnet-4-6"});
  CHECK_EQ(s.effort, std::string{""});  // status blob has no effort
  CHECK_EQ(s.tokens, -1LL);             // total_input_tokens only on statusline
  std::filesystem::remove_all(sr);
}

// Neither capture present → all-default (the honest "no data" state).
TEST(context_stats_absent_is_all_default) {
  const auto sr = makeRoot("/tmp/claude-bus-test-ctx-none-");
  const auto s = contextStatsFor(sr, "ghost");
  CHECK_EQ(s.pct, -1);
  CHECK_EQ(s.size_tokens, -1LL);
  CHECK_EQ(s.tokens, -1LL);
  CHECK_EQ(s.model, std::string{""});
}
