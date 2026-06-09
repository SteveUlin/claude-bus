#pragma once

// Readiness sentinel reader: derives an agent's at-boundary state from
// $STATE/ready/<agent>.json. A pure Readers-family derivation (no authority,
// reconstructs from files); state root is INJECTED (no global acquisition),
// making it unit-testable against a temp dir.
//
// The sentinel is written by the stop-hook (Slice 1b). This module is the
// read side only — Slice 1a. Until the write-hook ships, readReady returns
// nullopt for every agent and the delivery fallback path is taken.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bus {

struct Ready {
  std::string session_id;
  std::int64_t boundary_seq{0};
  std::int64_t ts_ms{0};
};

// Read $STATE/ready/<agent>.json (flat JSON {session_id, boundary_seq,
// ts_ms}). Returns nullopt if the file is absent or unparseable.
auto readReady(std::string_view state_root, std::string_view agent)
    -> std::optional<Ready>;

// Returns true when the sentinel is fresh:
//   now_ms - r.ts_ms < ttl_ms          (not expired)
//   AND r.ts_ms >= last_event_ms        (staleness fence: agent hasn't resumed
//                                        working since the boundary)
// last_event_ms is the agent's most-recent events.jsonl timestamp; a newer
// event means the sentinel predates the current turn and is stale.
auto readyFresh(const Ready& r, std::int64_t now_ms, std::int64_t ttl_ms,
                std::int64_t last_event_ms) -> bool;

}  // namespace bus
