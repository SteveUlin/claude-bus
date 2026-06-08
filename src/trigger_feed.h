// The P3 context-management trigger feed — shared derivation.
//
// Two axes per agent, because ctx-fill ALONE is the wrong trigger:
// intervening (compact/handoff) mid-turn drops the agent's planned tool
// calls (the mid-stream-dropped-turn failure). A context-manager must act
// only when it's URGENT *and* SAFE.
//   - URGENCY: ctx-fill% from $STATE/status (read-only — elodin's file).
//     Acts EARLY (P3 prior-art: 50-60%, not 95%).
//   - SAFETY:  boundary = "done" (a recent $STATE/done stamp — strongest,
//     agent-authored "I finished a unit of work") | "idle" (turn-axis
//     idle, no pending tool calls) | "none" (mid-WORKING/COMPACTING —
//     UNSAFE, queued tool calls).
//
// This header lifts the derivation out of `bus triggers` so the always-on
// `bus monitor` loop can reuse it: one derivation, two surfaces (the CLI
// table + the monitor's alarm-zone), and a continuous file refresh for
// elodin's decoupled P3 actuator. See docs/p3-trigger-feed.md.

#pragma once

#include "json_min.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace bus {

enum class Boundary { Done, Idle, None };
auto boundaryStr(Boundary b) -> std::string_view;
// Inverse bridge for deserialization ("done"|"idle"|"none"; unknown → None).
auto stringToBoundary(std::string_view s) -> Boundary;

// Glance recommendation: ACT (urgent + boundary != none) / WAIT (urgent +
// mid-turn) / OK (not urgent).
enum class Rec { Act, Wait, Ok };

struct Trig {
  std::string agent;
  int pct{-1};               // ctx-fill %, -1 = unknown (no status file)
  std::string source{"used_percentage"};  // which $STATE/status field pct
                                           // came from (the finer
                                           // "context_tokens" once the
                                           // P2 broker emits it)
  Boundary boundary{Boundary::Idle};
  std::int64_t done_ms{-1};  // ts of last done stamp, -1 = none
  std::int64_t idle_ms{-1};  // age since last event when idle, -1 = working
};

auto recOf(const Trig& t) -> Rec;

// Derive a trigger from the broker's per-agent state `entry` (the same
// snapshot the monitor + `bus triggers` already pull). Reads $STATE/status
// + $STATE/done read-only. `now` is ms-epoch.
auto deriveTrig(std::string_view agent, const json::Value& entry,
                std::int64_t now) -> Trig;

// Atomic write (tmp + rename) of $STATE/triggers/<agent>.json for elodin's
// P3 actuator — decoupled, no live round-trip. `computed_at_ms` lets the
// actuator treat a stale file as no-signal (the writer is a viewer, and
// viewers die here): stale → P3 sees no-signal → won't fire → safe.
auto writeTrigger(const Trig& t, std::int64_t now) -> void;

}  // namespace bus
