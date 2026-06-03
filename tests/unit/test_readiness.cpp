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

auto pane(bool ok, std::string mode) -> PaneState {
  PaneState p;
  p.ok = ok;
  p.mode = std::move(mode);
  return p;
}

}  // namespace

// ── the two unchanged wakeable shapes ──────────────────────────────────

TEST(wake_alive_ready_is_wakeable) {
  auto p = pane(true, "NORMAL");  // pane mode irrelevant when Alive+Ready
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Ready), &p));
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Ready), nullptr));
}

TEST(wake_compacting_is_wakeable) {
  CHECK(wakeReadyForMail(axes(ProcessAxis::Compacting, TurnAxis::None),
                         nullptr));
}

// ── the fix: fresh-idle boot at an INSERT prompt (harness-gap #4) ──────

TEST(wake_fresh_starting_at_insert_is_wakeable) {
  auto p = pane(true, "INSERT");
  CHECK(wakeReadyForMail(axes(ProcessAxis::Starting, TurnAxis::None), &p));
}

TEST(wake_stuck_boot_at_insert_is_wakeable) {
  // >30s idle fresh boot reads Stuck, but an INSERT prompt means ready.
  auto p = pane(true, "INSERT");
  CHECK(wakeReadyForMail(axes(ProcessAxis::Stuck, TurnAxis::None), &p));
}

// ── BOOT_STUCK preserved: a wedged boot (non-INSERT / no pane) is NOT woken ─

TEST(wake_wedged_boot_modal_not_wakeable) {
  auto p = pane(true, "LOCKED");  // a modal overlay, not an editable prompt
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Stuck, TurnAxis::None), &p));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Starting, TurnAxis::None), &p));
}

TEST(wake_boot_no_pane_state_not_wakeable) {
  auto p = pane(false, "INSERT");  // pane read failed → can't trust mode
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Stuck, TurnAxis::None), &p));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Starting, TurnAxis::None), nullptr));
}

TEST(wake_boot_normal_mode_not_wakeable) {
  auto p = pane(true, "NORMAL");
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Starting, TurnAxis::None), &p));
}

// ── other states are never woken (even at INSERT) ──────────────────────

TEST(wake_working_not_wakeable) {
  auto p = pane(true, "INSERT");
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Working), &p));
}

// Orchestrating = receptive mid-turn (Workflow within its lease). The gate
// flip: deliverable like Ready, unlike the true Working turn above. This is
// the activating half of kvothe's behavior-neutral state landing.
TEST(wake_orchestrating_is_wakeable) {
  auto p = pane(true, "INSERT");
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Orchestrating),
                         &p));
  CHECK(wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::Orchestrating),
                         nullptr));
}

TEST(wake_needs_input_not_wakeable) {
  auto p = pane(true, "INSERT");
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Alive, TurnAxis::NeedsInput), &p));
}

TEST(wake_gone_ended_new_not_wakeable) {
  auto p = pane(true, "INSERT");
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Gone, TurnAxis::None), &p));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::Ended, TurnAxis::None), &p));
  CHECK(!wakeReadyForMail(axes(ProcessAxis::New, TurnAxis::None), &p));
}
