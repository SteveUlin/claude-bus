#include "harness.h"
#include "recovery.h"

using namespace bus::delivery;

namespace {

// Small, explicit thresholds so the math is readable (no env dependence).
//   base=100ms, cap=10000ms, MaxR=3 / MaxT=1000ms, breaker cooldown=500ms.
auto th() -> RecoveryThresholds {
  RecoveryThresholds t;
  t.backoff_base_ms = 100;
  t.backoff_cap_ms = 10'000;
  t.maxt_ms = 1'000;
  t.maxr = 3;
  t.breaker_cooldown_ms = 500;
  return t;
}

}  // namespace

// ── per-signature exponential backoff ──────────────────────────────────────

TEST(backoff_doubles_per_attempt_then_caps) {
  RecoveryLedger l;
  const auto t = th();
  // Fire the same signature repeatedly at now=0; each fire doubles the window.
  recoveryRecord(l, "s", RecoveryAction::Nudge, 0, t);
  CHECK_EQ(l.sigs["s"].backoff_until_mono_ms, 100);  // base × 2^0
  recoveryRecord(l, "s", RecoveryAction::Nudge, 0, t);
  CHECK_EQ(l.sigs["s"].backoff_until_mono_ms, 200);  // base × 2^1
  recoveryRecord(l, "s", RecoveryAction::Nudge, 0, t);
  CHECK_EQ(l.sigs["s"].backoff_until_mono_ms, 400);  // base × 2^2
  // Keep firing until the doubling overshoots the cap; it clamps.
  for (int i = 0; i < 6; ++i)
    recoveryRecord(l, "s", RecoveryAction::Nudge, 0, t);
  CHECK_EQ(l.sigs["s"].backoff_until_mono_ms, 10'000);  // capped
}

TEST(backoff_gates_decide_until_window_elapses) {
  RecoveryLedger l;
  const auto t = th();
  recoveryRecord(l, "s", RecoveryAction::Nudge, 0, t);  // backoff_until = 100
  CHECK(!recoveryDecide(l, "s", RecoveryAction::Nudge, 50, t).allow);
  CHECK_EQ(recoveryDecide(l, "s", RecoveryAction::Nudge, 50, t).reason,
           std::string{"backoff"});
  CHECK(recoveryDecide(l, "s", RecoveryAction::Nudge, 100, t).allow);  // ==
  CHECK(recoveryDecide(l, "s", RecoveryAction::Nudge, 150, t).allow);
}

// ── MaxR/MaxT circuit breaker ──────────────────────────────────────────────

TEST(maxr_relaunches_in_window_trip_breaker) {
  RecoveryLedger l;
  const auto t = th();
  // 3 relaunches (MaxR) within MaxT=1000 → trips Open at the 3rd.
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 0, t),
           BreakerState::Closed);
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 10, t),
           BreakerState::Closed);
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 20, t),
           BreakerState::Open);
  CHECK_EQ(l.open_until_mono_ms, 520);  // 20 + cooldown(500)
}

TEST(breaker_open_denies_relaunch_until_cooldown) {
  RecoveryLedger l;
  const auto t = th();
  for (auto now : {0, 10, 20})
    recoveryRecord(l, "wedged", RecoveryAction::Relaunch, now, t);
  CHECK_EQ(l.breaker, BreakerState::Open);
  // At now=450 the per-sig backoff (until 420) has cleared, so the DENY is the
  // breaker, not backoff — assert the reason to prove which gate bit.
  auto d = recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 450, t);
  CHECK(!d.allow);
  CHECK_EQ(d.reason, std::string{"breaker-open"});
}

TEST(soft_actions_bypass_open_breaker) {
  RecoveryLedger l;
  const auto t = th();
  for (auto now : {0, 10, 20})
    recoveryRecord(l, "wedged", RecoveryAction::Relaunch, now, t);
  CHECK_EQ(l.breaker, BreakerState::Open);
  // A nudge/clear on a DIFFERENT signature (clean backoff) fires even while
  // the breaker is open — §6 "non-destructive may fire on one strong signal".
  CHECK(recoveryDecide(l, "idle", RecoveryAction::Nudge, 100, t).allow);
  CHECK(recoveryDecide(l, "idle", RecoveryAction::Clear, 100, t).allow);
}

TEST(stale_relaunches_prune_from_window_no_trip) {
  RecoveryLedger l;
  const auto t = th();  // MaxT=1000
  // Three relaunches each spaced > MaxT apart → window never holds ≥ MaxR.
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 0, t),
           BreakerState::Closed);
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 2'000, t),
           BreakerState::Closed);
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 4'000, t),
           BreakerState::Closed);
  CHECK_EQ(static_cast<int>(l.relaunch_mono_ms.size()), 1);  // only the newest
}

// ── half-open probe lifecycle ──────────────────────────────────────────────

TEST(cooldown_allows_one_probe_then_half_open) {
  RecoveryLedger l;
  const auto t = th();
  for (auto now : {0, 10, 20})
    recoveryRecord(l, "wedged", RecoveryAction::Relaunch, now, t);
  CHECK_EQ(l.breaker, BreakerState::Open);  // open_until = 520
  // Before cooldown: denied. At/after cooldown: one probe is eligible.
  CHECK(!recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 519, t).allow);
  CHECK(recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 520, t).allow);
  // Firing the probe moves to HalfOpen and re-arms the cooldown.
  CHECK_EQ(recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 520, t),
           BreakerState::HalfOpen);
  CHECK_EQ(l.open_until_mono_ms, 1'020);  // 520 + cooldown
}

TEST(half_open_denies_until_probe_window) {
  // Isolate the HalfOpen breaker branch: a HalfOpen ledger with no per-sig
  // backoff armed → the breaker window is the binding gate.
  RecoveryLedger l;
  const auto t = th();
  l.breaker = BreakerState::HalfOpen;
  l.open_until_mono_ms = 1'000;  // next probe slot
  auto d = recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 600, t);
  CHECK(!d.allow);
  CHECK_EQ(d.reason, std::string{"breaker-half-open"});
  CHECK(recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 1'000, t).allow);
}

TEST(half_open_probe_interval_grows_with_backoff) {
  RecoveryLedger l;
  const auto t = th();
  for (auto now : {0, 10, 20})
    recoveryRecord(l, "wedged", RecoveryAction::Relaunch, now, t);
  recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 520, t);  // probe → HO
  CHECK_EQ(l.breaker, BreakerState::HalfOpen);
  // After the probe BOTH gates re-arm. The per-sig backoff (1320) now exceeds
  // the breaker cooldown (open_until 1020), so backoff governs the next probe
  // — exponential CrashLoopBackOff on a persistently-failing probe. Defense in
  // depth: the more restrictive gate always wins.
  CHECK_EQ(l.sigs["wedged"].backoff_until_mono_ms, 1'320);
  CHECK(!recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 1'020, t).allow);
  CHECK(recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 1'320, t).allow);
}

TEST(half_open_success_closes_and_resets) {
  RecoveryLedger l;
  const auto t = th();
  for (auto now : {0, 10, 20})
    recoveryRecord(l, "wedged", RecoveryAction::Relaunch, now, t);
  recoveryRecord(l, "wedged", RecoveryAction::Relaunch, 520, t);  // → HalfOpen
  CHECK_EQ(l.breaker, BreakerState::HalfOpen);
  // Agent looked healthy this scan → probe succeeded → fully reset.
  recoveryObserveHealthy(l, 700, t);
  CHECK_EQ(l.breaker, BreakerState::Closed);
  CHECK_EQ(l.open_until_mono_ms, 0);
  CHECK_EQ(static_cast<int>(l.relaunch_mono_ms.size()), 0);
  CHECK_EQ(l.sigs["wedged"].attempts, 0);
  // A relaunch is freely allowed again post-recovery.
  CHECK(recoveryDecide(l, "wedged", RecoveryAction::Relaunch, 700, t).allow);
}

TEST(observe_healthy_noop_when_closed) {
  RecoveryLedger l;
  const auto t = th();
  // Closed breaker with armed backoff must NOT be wiped by a healthy scan —
  // else an agent oscillating healthy/unhealthy never accumulates backoff.
  recoveryRecord(l, "s", RecoveryAction::Nudge, 0, t);
  recoveryObserveHealthy(l, 50, t);
  CHECK_EQ(l.sigs["s"].backoff_until_mono_ms, 100);  // untouched
  CHECK_EQ(l.breaker, BreakerState::Closed);
}

// ── persistence: the breaker survives a (same-boot) broker restart ──────────

TEST(ledger_json_roundtrip_preserves_breaker) {
  RecoveryLedger l;
  const auto t = th();
  for (auto now : {0, 10, 20})
    recoveryRecord(l, "wedged", RecoveryAction::Relaunch, now, t);
  l.boot_id = "boot-abc";
  CHECK_EQ(l.breaker, BreakerState::Open);

  // Serialize → parse (the saveRecovery/loadRecovery on-disk path).
  auto reloaded = ledgerFromJson(ledgerToJson(l));
  CHECK_EQ(reloaded.boot_id, std::string{"boot-abc"});
  CHECK_EQ(reloaded.breaker, BreakerState::Open);
  CHECK_EQ(reloaded.open_until_mono_ms, l.open_until_mono_ms);
  CHECK_EQ(reloaded.sigs["wedged"].attempts, 3);

  // The whole point: after a restart the breaker is STILL open, so a
  // crash-looping agent does not get a fresh MaxR budget.
  auto d = recoveryDecide(reloaded, "wedged", RecoveryAction::Relaunch, 450, t);
  CHECK(!d.allow);
  CHECK_EQ(d.reason, std::string{"breaker-open"});
}
