#include "harness.h"
#include "ready.h"

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

// readReady on an empty root returns nullopt — file absent.
TEST(readReady_absent_is_nullopt) {
  const auto sr = makeRoot("/tmp/claude-bus-test-ready-absent-");
  const auto r = readReady(sr, "alice");
  CHECK(!r.has_value());
  std::filesystem::remove_all(sr);
}

// readReady parses all three fields from a well-formed sentinel file.
TEST(readReady_parses_fields) {
  const auto sr = makeRoot("/tmp/claude-bus-test-ready-parse-");
  writeFile(sr + "/ready/alice.json",
            R"({"session_id":"s1","boundary_seq":7,"ts_ms":1000})");
  const auto r = readReady(sr, "alice");
  CHECK(r.has_value());
  CHECK_EQ(r->session_id, std::string{"s1"});
  CHECK_EQ(r->boundary_seq, 7LL);
  CHECK_EQ(r->ts_ms, 1000LL);
  std::filesystem::remove_all(sr);
}

// Within TTL and not superseded by a newer event → fresh.
TEST(readyFresh_within_ttl_and_newer_than_event) {
  const Ready r{"s1", 0, 1000};
  CHECK(readyFresh(r, /*now=*/1500, /*ttl=*/1000, /*last_event=*/900));
}

// Past the TTL → not fresh even if the event fence is satisfied.
TEST(readyFresh_beyond_ttl) {
  const Ready r{"s1", 0, 1000};
  CHECK(!readyFresh(r, /*now=*/3000, /*ttl=*/1000, /*last_event=*/900));
}

// A newer event supersedes the sentinel → stale even within TTL.
TEST(readyFresh_stale_vs_newer_event) {
  const Ready r{"s1", 0, 1000};
  CHECK(!readyFresh(r, /*now=*/1500, /*ttl=*/1000, /*last_event=*/2000));
}
