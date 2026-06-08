#pragma once

// TokenWatcher — transcript-token scanning, extracted from delivery::Loop.
//
// For each live agent, tails its transcript JSONL, computes context occupancy
// from the last assistant turn's usage block, and writes
// $STATE/status/<agent>.json — the CTX% source `bus monitor` reads.
//
// The scan is rate-limited to once every 5 s internally so the caller can
// invoke scan() on every broker tick without worry. The 5-second cadence and
// all per-agent offset state live here; delivery::Loop no longer touches them.

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace bus {

class TokenWatcher {
 public:
  explicit TokenWatcher(std::string state_dir);

  // Scan live agents' transcripts; rate-limited to once every 5 s.
  // Reads CLAUDE_BUS_CTX_WINDOW for the denominator (default 200k).
  auto scan() -> void;

  // Erase per-agent scan state for any agent NOT in live_agents.
  // Called by Loop::pruneDeadAgents after it builds the live set.
  auto pruneDead(const std::set<std::string>& live_agents) -> void;

 private:
  // Per-agent incremental-read state. Offset is byte-stable across polls;
  // path reset triggers a full re-read (new session after /clear or /compact).
  struct TokenScanState {
    std::string path;
    std::int64_t offset{0};
    std::int64_t last_tokens{-1};
    std::string last_model;  // last non-<synthetic> assistant model seen
  };

  std::string state_dir_;
  std::map<std::string, TokenScanState> token_scan_;
  std::int64_t token_scan_last_ms_{0};
};

}  // namespace bus
