#include "agent_status.h"
#include "harness.h"

using namespace bus;

namespace {

auto axes(ProcessAxis p, TurnAxis t) -> AgentAxes {
  AgentAxes ax;
  ax.process = p;
  ax.turn = t;
  return ax;
}

}  // namespace

// wakeReadyForMail now takes a `ready_fresh` bool — the readiness sentinel
// ($STATE/ready, the agent's own at-a-boundary signal) — instead of a
// PaneState*. true = a fresh sentinel (replacing the old "editable INSERT
// prompt" pane read); the resolve-from-sentinel-or-pane-fallback happens in the
// caller (delivery's doorbell, recovery_actor).

// ── the two unchanged wakeable shapes (ready_fresh irrelevant) ─────────────

TEST(wake_alive_ready_is_wakeable) {
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Ready), false));
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Ready), true));
}

TEST(wake_compacting_is_wakeable) {
  CHECK(wakeReadyForMail(axes(ProcessAxis::Compacting, TurnAxis::None), false));
}

// ── the fix: a fresh-idle boot with a fresh sentinel (harness-gap #4) ──────

TEST(wake_fresh_starting_ready_is_wakeable) {
  CHECK(wakeReadyForMail(axes(ProcessAxis::Starting, TurnAxis::None), true));
}

TEST(wake_stuck_boot_ready_is_wakeable) {
  // A >30s-idle fresh boot reads Stuck, but a fresh sentinel means ready.
  CHECK(wakeReadyForMail(axes(ProcessAxis::Stuck, TurnAxis::None), true));
}

// ── BOOT_STUCK preserved: a wedged boot has no fresh sentinel → NOT woken ──
// (covers the former modal / pane-read-failed / non-INSERT cases — all now
// collapse to "ready_fresh == false".)

TEST(wake_wedged_boot_no_sentinel_not_wakeable) {
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Stuck, TurnAxis::None), false));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Starting, TurnAxis::None), false));
}

// ── other states are never woken (even with a fresh sentinel) ──────────────

TEST(wake_working_not_wakeable) {
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Working), true));
}

// Orchestrating = receptive mid-turn (Workflow within its lease). The gate
// flip: deliverable like Ready, unlike the true Working turn above.
TEST(wake_orchestrating_is_wakeable) {
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Orchestrating),
                         false));
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Orchestrating),
                         true));
}

TEST(wake_needs_input_not_wakeable) {
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::NeedsInput), true));
}

TEST(wake_gone_ended_new_not_wakeable) {
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Gone, TurnAxis::None), true));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Ended, TurnAxis::None), true));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::New, TurnAxis::None), true));
}
