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

// The wake gate, verbatim from delivery.cpp (inverted to a positive "is
// this agent doorbell-eligible?"). If this drifts from the broker's gate
// the verification is worthless, so keep them identical. Two wakeable
// shapes: Alive+Ready (idle at the prompt) OR Compacting (a
// SessionStart(compact) just FINISHED — also idle at the prompt).
auto doorbellReady(const AgentInfo& a) -> bool {
  const auto ax = computeAxes(a, /*unread=*/0, kNow, /*pane_exists=*/true);
  return (ax.process == ProcessAxis::Alive && ax.turn == TurnAxis::Ready) ||
         ax.process == ProcessAxis::Compacting;
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

// ─── THE POST-COMPACTION STRAND (the live C4 bug) — NOW FIXED ───
// bast, elodin, AND kvothe all stranded post-compaction this session:
// mail queued, no doorbell-wake, no strand alarm, human had to TTY-inject.
//
// Root cause: /compact emits SessionStart(source=compact), which
// computeAxes maps to ProcessAxis::Compacting (NOT Alive) — see
// agent_status.cpp:271-274. SessionStart(compact) fires when compaction
// FINISHES (PreCompact precedes it by the whole compaction duration), so
// the agent is then idle at the prompt — but the old gate required
// process==Alive and excluded Compacting, so the doorbell never rang and
// the mail stranded (no follow-up event ever comes for an idle agent).
//
// The fix admits Compacting to the wake gate (delivery.cpp), so a
// post-compaction idle agent IS woken. (Paired with inbox-drain.sh no
// longer draining on source=compact, so the mail is still queued for that
// wake to deliver — the gate alone can't redeliver mail the SessionStart
// drain already consumed into a context that never surfaces it.) This was
// the acceptance assertion: it FLIPPED from !doorbellReady to doorbellReady
// when the fix landed.
TEST(doorbell_wakes_post_compaction_idle) {
  const auto compacted = mk("SessionStart", "compact", "", "", ago(2));
  // Still classified Compacting — the monitor display is intentionally
  // unchanged (the agent did just compact)...
  CHECK_EQ(axisName(computeAxes(compacted, 0, kNow, true).process),
           std::string_view{"compacting"});
  // ...but the wake gate now ADMITS it, so the doorbell rings and the
  // queued mail delivers. No longer a strand.
  CHECK(doorbellReady(compacted));
  // Holds regardless of age — Compacting is always post-compaction-idle.
  CHECK(doorbellReady(mk("SessionStart", "compact", "", "", ago(3600))));
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
