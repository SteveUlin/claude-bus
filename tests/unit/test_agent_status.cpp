#include "harness.h"
#include "agent_status.h"

#include <string>
#include <string_view>

using namespace bus;

namespace {

// Fixed "now" so age math is deterministic and the tests are pure.
constexpr std::int64_t kNow = 1'000'000'000'000;

auto ago(std::int64_t seconds) -> std::int64_t { return kNow - seconds * 1000; }

auto mk(std::string event, std::string source = "", std::string tool = "",
        std::int64_t ts_ms = kNow) -> AgentInfo {
  AgentInfo a;
  a.last.event = std::move(event);
  a.last.source = std::move(source);
  a.last.tool = std::move(tool);
  a.last.ts_ms = ts_ms;
  return a;
}

auto procOf(const AgentInfo& a, bool pane = true) -> std::string_view {
  return axisName(computeAxes(a, 0, kNow, pane).process);
}
auto turnOf(const AgentInfo& a, bool pane = true) -> std::string_view {
  return axisName(computeAxes(a, 0, kNow, pane).turn);
}

}  // namespace

// ─── BOOT_STUCK classifier (regression-locks the recent fix) ───
// The "no follow-up after SessionStart = boot hang" heuristic must apply
// ONLY to source=startup. resume/clear land at an active prompt; compact
// runs an unbounded internal task. Before the fix, clear/compact flickered
// through BOOT_STUCK on quiet fleets after every broker auto-clear.

TEST(boot_startup_old_is_stuck) {
  // A fresh startup with no follow-up for >30s is a genuine boot hang.
  CHECK_EQ(procOf(mk("SessionStart", "startup", "", ago(40))),
           std::string_view{"stuck"});
}

TEST(boot_startup_recent_is_starting) {
  CHECK_EQ(procOf(mk("SessionStart", "startup", "", ago(5))),
           std::string_view{"starting"});
}

TEST(boot_resume_is_alive_regardless_of_age) {
  // Resume inherits the prior prompt; absence of a follow-up is normal.
  CHECK_EQ(procOf(mk("SessionStart", "resume", "", ago(600))),
           std::string_view{"alive"});
}

TEST(boot_clear_is_alive_not_stuck) {
  // The fix: /clear lands at a fresh prompt, never boot-stuck.
  CHECK_EQ(procOf(mk("SessionStart", "clear", "", ago(600))),
           std::string_view{"alive"});
}

TEST(boot_compact_is_compacting_not_stuck) {
  // The fix: /compact is unbounded internal work; stays Compacting until a
  // real follow-up event, never flips to stuck on age alone.
  CHECK_EQ(procOf(mk("SessionStart", "compact", "", ago(600))),
           std::string_view{"compacting"});
}

TEST(boot_clear_maps_to_idle_state_when_pane_exists) {
  // End-to-end through computeState: a cleared agent reads IDLE, not
  // BOOT_STUCK — the bug sulin reported (quiet agents flickering stuck).
  const auto st = computeState(mk("SessionStart", "clear", "", ago(600)), 0,
                               kNow, /*pane_exists=*/true);
  CHECK_EQ(stateName(st), std::string_view{"IDLE"});
}

// ─── Turn axis: ready / working / needs-input ───

TEST(stop_is_ready_then_idle_or_hasmail) {
  const auto a = mk("Stop", "", "", ago(2));
  CHECK_EQ(turnOf(a), std::string_view{"ready"});
  CHECK_EQ(stateName(computeState(a, 0, kNow, true)),
           std::string_view{"IDLE"});
  CHECK_EQ(stateName(computeState(a, 3, kNow, true)),
           std::string_view{"HAS_MAIL"});
}

TEST(askuserquestion_is_needs_input) {
  CHECK_EQ(turnOf(mk("PreToolUse", "", "AskUserQuestion", ago(2))),
           std::string_view{"needs_input"});
}

TEST(resume_turn_is_ready) {
  const auto a = mk("SessionStart", "resume", "", ago(2));
  CHECK_EQ(procOf(a), std::string_view{"alive"});
  CHECK_EQ(turnOf(a), std::string_view{"ready"});
}

// ─── Process axis: pane existence ───

TEST(no_pane_non_sessionend_is_gone) {
  CHECK_EQ(procOf(mk("PostToolUse", "", "Bash", ago(2)), /*pane=*/false),
           std::string_view{"gone"});
}

TEST(sessionend_is_ended) {
  CHECK_EQ(procOf(mk("SessionEnd", "", "", ago(2))),
           std::string_view{"ended"});
}

// ─── The mid-stream dropped turn (the documented GAP) ───
// computeAxes is f(last_event), not fold(events): it keeps only the latest
// event, so a PreToolUse with no matching PostToolUse (the "dropped turn")
// carries NO "open tool pending" fact. The turn looks like ordinary work
// until the 5-min wall-clock threshold finally flips it to Stuck — the
// machine cannot distinguish "mid-tool, healthy" from "tool dropped, wedged"
// until time passes. This test asserts that CURRENT behavior and marks the
// gap that roadmap 2.5 (a true fold carrying open_tool) would close.
TEST(dropped_turn_invisible_until_wallclock_threshold) {
  // PreToolUse:Bash, no PostToolUse — recent: indistinguishable from work.
  CHECK_EQ(turnOf(mk("PreToolUse", "", "Bash", ago(60))),
           std::string_view{"working"});
  // Only after 5 min does age alone surface it as stuck.
  CHECK_EQ(turnOf(mk("PreToolUse", "", "Bash", ago(6 * 60))),
           std::string_view{"stuck"});
  // NOTE: when 2.5 lands, an open-tool accumulator should flag the dropped
  // turn immediately rather than waiting out the wall clock — update this
  // test to assert detection at ago(60).
}

// ─── Orchestrating: mid-turn but receptive (Workflow lease) ───
// Lease window is 90 s (internal). ago(30) is fresh; ago(120) has decayed.

TEST(orchestrating_within_lease) {
  // Mid-turn work + a Workflow dispatched 30 s ago → Orchestrating, the
  // receptive sibling of Working.
  auto a = mk("PreToolUse", "", "Bash", ago(20));
  a.last_orchestration_ms = ago(30);
  CHECK_EQ(turnOf(a), std::string_view{"orchestrating"});
}

TEST(orchestrating_decays_to_working) {
  // Same mid-turn work, but the last Workflow was 120 s ago — lease expired,
  // so it reads plain Working again (auto-decay, no stuck-Orchestrating).
  auto a = mk("PreToolUse", "", "Bash", ago(20));
  a.last_orchestration_ms = ago(120);
  CHECK_EQ(turnOf(a), std::string_view{"working"});
}

TEST(orchestrating_does_not_override_ready) {
  // A Stop with a fresh lease is still Ready (at the prompt) — the override
  // touches ONLY Working, never the receptive-already Ready state.
  auto a = mk("Stop", "", "", ago(10));
  a.last_orchestration_ms = ago(30);
  CHECK_EQ(turnOf(a), std::string_view{"ready"});
}

TEST(orchestrating_stuck_outranks_lease) {
  // Mid-turn but stale > 5 min → Stuck wins over any lease (a genuine stall
  // is not "receptive"). The 90 s lease has expired by then anyway.
  auto a = mk("PreToolUse", "", "Bash", ago(6 * 60));
  a.last_orchestration_ms = ago(6 * 60);
  CHECK_EQ(turnOf(a), std::string_view{"stuck"});
}
