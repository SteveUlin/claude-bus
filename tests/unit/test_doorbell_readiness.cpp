#include "harness.h"
#include "agent_status.h"

#include <string>
#include <string_view>

// Doorbell-readiness verification (roadmap C4 — off-TTY + doorbell
// objective verification).
//
// The doorbell wakes an idle off-TTY agent that has queued mail by
// firing one sentinel submit ([bus-wake]); the agent's drain hook then
// pulls its inbox. The broker's wake gate (delivery.cpp, maybeWakeIdle-
// OffTty) admits an agent ONLY when:
//
//     computeAxes(...).process == ProcessAxis::Alive
//  && computeAxes(...).turn    == TurnAxis::Ready
//
// `computeAxes` is a pure function, so that exact predicate is unit-
// testable in isolation — no broker, no zellij, no clock. These tests
// pin the predicate across the event/source matrix that matters for
// waking, and REPRODUCE the live post-compaction strand at the logic
// level.

using namespace bus;

namespace {

constexpr std::int64_t kNow = 1'000'000'000'000;

auto ago(std::int64_t seconds) -> std::int64_t { return kNow - seconds * 1000; }

auto mk(std::string event, std::string source = "", std::string tool = "",
        std::string notif = "", std::int64_t ts_ms = kNow) -> AgentInfo {
  AgentInfo a;
  a.last.event = std::move(event);
  a.last.source = std::move(source);
  a.last.tool = std::move(tool);
  a.last.notification_type = std::move(notif);
  a.last.ts_ms = ts_ms;
  return a;
}

// The wake gate, verbatim from delivery.cpp:1033 (inverted to a positive
// "is this agent doorbell-eligible?"). If this drifts from the broker's
// gate the verification is worthless, so keep them identical.
auto doorbellReady(const AgentInfo& a) -> bool {
  const auto ax = computeAxes(a, /*unread=*/0, kNow, /*pane_exists=*/true);
  return ax.process == ProcessAxis::Alive && ax.turn == TurnAxis::Ready;
}

}  // namespace

// ─── Positive cases: an idle agent at the prompt IS wakeable ───
// These are the states a healthy off-TTY agent rests in between turns.
// Each must be doorbell-eligible or queued mail strands.

TEST(doorbell_ready_after_stop) {
  // Turn finished, sitting idle at the prompt — the common case.
  CHECK(doorbellReady(mk("Stop", "", "", "", ago(2))));
}

TEST(doorbell_ready_on_idle_notification) {
  CHECK(doorbellReady(mk("Notification", "", "", "idle_prompt", ago(2))));
}

TEST(doorbell_ready_after_resume) {
  // A just-relaunched agent (resume) lands at an active prompt. The
  // earlier prod failure was this case stranding; it must be wakeable.
  CHECK(doorbellReady(mk("SessionStart", "resume", "", "", ago(2))));
}

TEST(doorbell_ready_after_clear) {
  CHECK(doorbellReady(mk("SessionStart", "clear", "", "", ago(2))));
}

// ─── THE POST-COMPACTION STRAND (the live C4 bug) ───
// bast, elodin, AND kvothe all stranded post-compaction this session:
// mail queued, no doorbell-wake, no strand alarm, human had to TTY-inject.
//
// Root cause, code-proven here: /compact emits SessionStart(source=
// compact), which computeAxes maps to ProcessAxis::Compacting (NOT
// Alive) — see agent_status.cpp:271-274. The wake gate requires
// process==Alive, so a post-compaction idle agent is EXCLUDED from the
// doorbell. agent_status.cpp:273 says it "stays Compacting until a
// follow-up event arrives" — but an idle off-TTY agent at the prompt
// fires NO follow-up event, and the one mechanism that would wake it
// (the doorbell) is gated off. Permanent strand until a human intervenes.
//
// This asserts the CURRENT (broken) behavior so the gap is executable
// and pinned. When the fix lands (compact made wake-eligible once the
// summary completes, or the gate widened to admit Compacting-with-mail),
// FLIP this to CHECK(doorbellReady(...)) — that flip IS the fix's
// acceptance test.
TEST(doorbell_strands_post_compaction_REPRODUCES_BUG) {
  const auto compacted = mk("SessionStart", "compact", "", "", ago(2));
  // Pinned: the agent is classified Compacting, not Alive...
  CHECK_EQ(axisName(computeAxes(compacted, 0, kNow, true).process),
           std::string_view{"compacting"});
  // ...so the doorbell gate rejects it and the mail strands.
  CHECK(!doorbellReady(compacted));
  // Age does not rescue it: an hour later it is still Compacting, still
  // unwakeable. (Stays until a real follow-up event that never comes.)
  CHECK(!doorbellReady(mk("SessionStart", "compact", "", "", ago(3600))));
}

// ─── Other negative cases: NOT at the prompt → correctly not woken ───
// These are genuinely-not-idle states; excluding them is correct, not a
// bug. They guard against a future fix over-widening the gate.

TEST(doorbell_skips_booting_agent) {
  // source=startup, still in the boot grace window — not at a prompt yet.
  CHECK(!doorbellReady(mk("SessionStart", "startup", "", "", ago(5))));
}

TEST(doorbell_skips_working_agent) {
  // Mid-turn (recent non-Stop event) — waking would interrupt work.
  CHECK(!doorbellReady(mk("PreToolUse", "", "Bash", "", ago(30))));
}

TEST(doorbell_skips_needs_input_agent) {
  // Blocked on AskUserQuestion — a [bus-wake] submit would answer the
  // question with the sentinel. Correctly excluded.
  CHECK(!doorbellReady(mk("PreToolUse", "", "AskUserQuestion", "", ago(2))));
}

TEST(doorbell_skips_permission_prompt) {
  CHECK(!doorbellReady(
      mk("Notification", "", "", "permission_prompt", ago(2))));
}
