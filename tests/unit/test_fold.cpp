#include "agent_status.h"
#include "harness.h"

#include <string>

using namespace bus;

namespace {

// Apply one event to the accumulator the way readAgents does: assign
// info.last, then fold.
auto apply(AgentInfo& a, std::string event, std::int64_t ts,
           std::string tool = "", std::string notif = "") -> void {
  a.last.event = std::move(event);
  a.last.tool = std::move(tool);
  a.last.notification_type = std::move(notif);
  a.last.ts_ms = ts;
  foldTurnState(a);
}

}  // namespace

// ─── turn open / close ───────────────────────────────────────────────

TEST(fold_user_prompt_opens_turn) {
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  CHECK_EQ(a.turn_start_ms, std::int64_t{1000});
  CHECK_EQ(a.open_tool, std::string{});
}

TEST(fold_stop_closes_turn) {
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "Stop", 2000);
  CHECK_EQ(a.turn_start_ms, std::int64_t{0});
  CHECK_EQ(a.open_tool, std::string{});
}

TEST(fold_idle_notification_closes_turn) {
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "Notification", 2000, "", "idle_prompt");
  CHECK_EQ(a.turn_start_ms, std::int64_t{0});
}

TEST(fold_permission_notification_keeps_turn_open) {
  // A permission prompt is mid-turn, not back-at-prompt.
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "Notification", 2000, "", "permission_prompt");
  CHECK_EQ(a.turn_start_ms, std::int64_t{1000});
}

// ─── tool open / close ───────────────────────────────────────────────

TEST(fold_pretool_opens_tool) {
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "PreToolUse", 1500, "Bash");
  CHECK_EQ(a.open_tool, std::string{"Bash"});
  CHECK_EQ(a.open_tool_since_ms, std::int64_t{1500});
  CHECK_EQ(a.turn_start_ms, std::int64_t{1000});  // turn still open
}

TEST(fold_posttool_closes_tool_turn_stays_open) {
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "PreToolUse", 1500, "Bash");
  apply(a, "PostToolUse", 1800, "Bash");
  CHECK_EQ(a.open_tool, std::string{});       // tool closed
  CHECK_EQ(a.open_tool_since_ms, std::int64_t{0});
  CHECK_EQ(a.turn_start_ms, std::int64_t{1000});  // model still working
}

TEST(fold_new_prompt_clears_stale_open_tool) {
  // A fresh turn must not inherit a previous turn's dangling tool.
  AgentInfo a;
  apply(a, "PreToolUse", 1500, "Bash");
  apply(a, "UserPromptSubmit", 3000);
  CHECK_EQ(a.open_tool, std::string{});
  CHECK_EQ(a.turn_start_ms, std::int64_t{3000});
}

// ─── the signatures D8 exists to make visible ────────────────────────

TEST(fold_wedged_tool_signature) {
  // PreToolUse with no PostToolUse: the call is in flight. open_tool +
  // since survive, so escalation can date a wedged Bash precisely instead
  // of seeing a bare "old PreToolUse".
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "PreToolUse", 1500, "Bash");
  // ...no PostToolUse, no Stop...
  CHECK_EQ(a.open_tool, std::string{"Bash"});
  CHECK_EQ(a.open_tool_since_ms, std::int64_t{1500});
  CHECK_EQ(a.turn_start_ms, std::int64_t{1000});
}

TEST(fold_dropped_turn_signature) {
  // UserPromptSubmit (mail injected) → the model streams text but the
  // planned tool call is dropped: no PreToolUse, no Stop. The turn is
  // OPEN with NO tool ever started — the dropped-turn fingerprint, now
  // explicit (turn_start set, open_tool empty) rather than just "last
  // event was UserPromptSubmit a while ago".
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  CHECK_EQ(a.turn_start_ms, std::int64_t{1000});
  CHECK_EQ(a.open_tool, std::string{});
}

TEST(fold_session_boundaries_reset) {
  AgentInfo a;
  apply(a, "UserPromptSubmit", 1000);
  apply(a, "PreToolUse", 1500, "Bash");
  apply(a, "SessionEnd", 2000);
  CHECK_EQ(a.turn_start_ms, std::int64_t{0});
  CHECK_EQ(a.open_tool, std::string{});
  apply(a, "PreToolUse", 2500, "Read");
  apply(a, "SessionStart", 2600);  // new session wipes mid-turn state
  CHECK_EQ(a.turn_start_ms, std::int64_t{0});
  CHECK_EQ(a.open_tool, std::string{});
}
