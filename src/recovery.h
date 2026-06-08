#pragma once

// P2 auto-recovery: the per-agent recovery ledger + the breaker/backoff
// guards (docs/broker-auto-recovery.md §6, §7). Carved out of delivery as a
// PURE unit — no Loop, no socket, no real clock — so the correctness-critical
// guard logic is unit-testable with synthetic monotonic timestamps (the
// seam-redesign litmus: a seam is real iff it's testable without a broker).
// The Loop's I/O wrappers (loadRecovery/saveRecovery, the recovery_ member)
// stay in delivery; they consume these types and functions.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "json_min.h"

namespace bus::delivery {

// ── Recovery mode (CLAUDE_BUS_AUTO_RECOVERY) ─────────────────────────────────
// off / "0"    — no auto-recovery behavior (0 is a legacy alias for off).
// observe      — log what would fire ("would-recover"); default.
// soft         — log + act on non-destructive rows (R1 /clear).
// on           — log + act on all rows including relaunch (Phase C).
enum class RecoveryMode { Off, Observe, Soft, On };

// Read CLAUDE_BUS_AUTO_RECOVERY once and return the mode. Returns Observe when
// the variable is unset or empty. fatal() on an unrecognized non-empty value —
// a misconfigured mode is a programmer/config error.
auto recoveryModeFromEnv() -> RecoveryMode;

// ── Ledger (§6.1, §7) ────────────────────────────────────────────────────
// Per-agent recovery state, persisted to $STATE/recovery/<agent>.json so the
// breaker survives a broker restart (a crash-looping agent that bounced the
// broker must NOT get a fresh MaxR budget). ALL timestamps are MONOTONIC ms
// (steady_clock == CLOCK_MONOTONIC on Linux): continuous across same-boot
// processes (restart preserves the windows), reset on reboot via the boot_id
// tag (mismatch → clear). Never wall-clock — a suspend/resume jump must not
// perturb the breaker.
enum class BreakerState { Closed, Open, HalfOpen };

// Per-signature exponential-backoff state (one row × one agent).
struct RecoverySig {
  std::int64_t last_fired_mono_ms{0};
  std::int32_t attempts{0};
  std::int64_t backoff_until_mono_ms{0};
};

struct RecoveryLedger {
  std::string boot_id;                         // boot this state belongs to
  std::map<std::string, RecoverySig> sigs;     // keyed by signature id
  std::vector<std::int64_t> relaunch_mono_ms;  // MaxR/MaxT rolling window
  BreakerState breaker{BreakerState::Closed};
  std::int64_t open_until_mono_ms{0};  // half-open probe gate
};

// Monotonic now (ms) — steady_clock, suspend-immune ordering for the engine.
auto nowMonoMs() -> std::int64_t;
// Current boot's UUID (/proc/sys/kernel/random/boot_id); "" if unreadable.
auto readBootId() -> std::string;
// Ledger <-> JSON (persistence roundtrips through these).
auto ledgerToJson(const RecoveryLedger& l) -> json::Value;
auto ledgerFromJson(const json::Value& v) -> RecoveryLedger;

// ── Breaker/backoff guards (§6) — pure functions over a RecoveryLedger ─────
// The recovery actions (R1-clear / R2,R3-nudge / R4-relaunch) will call
// recoveryDecide before acting and recoveryRecord after; until they are wired
// (increment c) these are exercised only by the unit tests.
enum class RecoveryAction { Clear, Nudge, Relaunch };

struct RecoveryThresholds {
  std::int64_t backoff_base_ms{30'000};           // backoff = base × 2^attempts
  std::int64_t backoff_cap_ms{30 * 60'000};       // clamp the doubling
  std::int64_t maxt_ms{10 * 60'000};              // MaxR/MaxT rolling window
  std::int32_t maxr{3};                           // relaunches/window → trip
  std::int64_t breaker_cooldown_ms{10 * 60'000};  // open → probe slot
};

// Defaults overlaid with CLAUDE_BUS_RECOVERY_* env knobs (positive ints only).
auto recoveryThresholds() -> RecoveryThresholds;

struct GuardDecision {
  bool allow{false};
  std::string
      reason;  // when denied: backoff | breaker-open | breaker-half-open
};

// May this (signature, action) fire for this agent NOW? Pure/const. Backoff
// gates every action; the MaxR/MaxT breaker additionally gates Relaunch.
auto recoveryDecide(const RecoveryLedger& l, const std::string& sig,
                    RecoveryAction action, std::int64_t now_mono,
                    const RecoveryThresholds& th) -> GuardDecision;

// Record that an action FIRED: arm the per-signature backoff; for Relaunch,
// push the MaxR/MaxT window and drive the breaker state machine. Returns the
// breaker state AFTER the mutation — a caller escalates to inbox-ops on a
// fresh transition into Open.
auto recoveryRecord(RecoveryLedger& l, const std::string& sig,
                    RecoveryAction action, std::int64_t now_mono,
                    const RecoveryThresholds& th) -> BreakerState;

// Agent looked HEALTHY this scan (no destructive signature matched): close an
// in-flight half-open probe, clear the thrash window, reset per-signature
// backoff. The "success re-closes" transition.
auto recoveryObserveHealthy(RecoveryLedger& l, std::int64_t now_mono,
                            const RecoveryThresholds& th) -> void;

}  // namespace bus::delivery
