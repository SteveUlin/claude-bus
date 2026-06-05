#include "harness.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent_status.h"
#include "policy.h"
#include "recovery_actor.h"

using namespace bus;
using namespace bus::delivery;

namespace {

// A canned actor: returns a fixed action list, ignores the context. Proves the
// engine's fan-out without standing up a real actor.
class FakeActor : public policy::PolicyActor {
 public:
  FakeActor(std::string n, std::vector<policy::PolicyAction> acts)
      : name_{std::move(n)}, acts_{std::move(acts)} {}
  auto name() const -> std::string_view override { return name_; }
  auto evaluate(const policy::PolicyContext&)
      -> std::vector<policy::PolicyAction> override {
    return acts_;
  }

 private:
  std::string name_;
  std::vector<policy::PolicyAction> acts_;
};

auto enqueue(std::string topic, std::string body) -> policy::PolicyAction {
  policy::PolicyAction a;
  a.kind = policy::PolicyAction::Kind::Enqueue;
  a.topic = std::move(topic);
  a.body = std::move(body);
  return a;
}

auto mkInfo(std::string event, std::int64_t ts_ms) -> AgentInfo {
  AgentInfo info;
  info.last.event = std::move(event);
  info.last.ts_ms = ts_ms;
  return info;
}

auto normalPane() -> std::function<PaneState(const std::string&)> {
  return [](const std::string&) {
    PaneState p;
    p.ok = true;
    p.mode = "NORMAL";  // not INSERT → not awaiting input
    return p;
  };
}

}  // namespace

// ── PolicyEngine fan-out ───────────────────────────────────────────────────

TEST(engine_fans_out_in_registration_order) {
  policy::PolicyEngine eng;
  std::vector<policy::PolicyAction> a;
  a.push_back(enqueue("t1", "b1"));
  std::vector<policy::PolicyAction> b;
  b.push_back(enqueue("t2", "b2"));
  b.push_back(enqueue("t3", "b3"));
  eng.registerActor(std::make_unique<FakeActor>("a", std::move(a)));
  eng.registerActor(std::make_unique<FakeActor>("b", std::move(b)));

  policy::PolicyContext ctx;
  auto out = eng.evaluate(ctx);
  CHECK_EQ(static_cast<int>(out.size()), 3);
  CHECK_EQ(out[0].body, std::string{"b1"});  // actor a first
  CHECK_EQ(out[1].body, std::string{"b2"});  // then actor b, in order
  CHECK_EQ(out[2].body, std::string{"b3"});
}

// ── RecoveryActor::evaluate (the litmus: no broker, synthetic context) ──────

TEST(recovery_actor_observe_flags_wedged_not_healthy) {
  std::filesystem::remove_all("/tmp/bus_test_policy_observe");
  setenv("CLAUDE_BUS_WEDGE_BUDGET_MS", "1000", 1);
  const std::int64_t now = 1'000'000;
  RecoveryActor actor{"/tmp/bus_test_policy_observe", "observe"};

  // wedged: looks busy (Working) + transcript stale + pane not awaiting input.
  AgentInfo wedged = mkInfo("PreToolUse", now - 1000);
  // healthy: idle at a ready prompt, fresh transcript, recent Stop.
  AgentInfo healthy = mkInfo("Stop", now - 1000);

  policy::PolicyContext ctx;
  ctx.now_wall_ms = now;
  ctx.now_mono_ms = now;
  ctx.pane = normalPane();

  policy::AgentSnapshot w;
  w.name = "wedged";
  w.info = &wedged;
  w.axes.turn = TurnAxis::Working;
  w.axes.process = ProcessAxis::Alive;
  w.transcript_age_ms = 10'000;  // > wedge_budget(1000) → stale
  ctx.agents.push_back(w);

  policy::AgentSnapshot h;
  h.name = "healthy";
  h.info = &healthy;
  h.axes.turn = TurnAxis::Ready;
  h.axes.process = ProcessAxis::Alive;
  h.transcript_age_ms = 100;  // fresh
  ctx.agents.push_back(h);

  auto out = actor.evaluate(ctx);
  CHECK_EQ(static_cast<int>(out.size()), 1);  // only the wedged one
  CHECK_EQ(out[0].topic, std::string{"audit"});
  CHECK_EQ(out[0].protocol, std::string{"would-recover"});
  CHECK(out[0].body.find("agent=wedged") != std::string::npos);
  CHECK(out[0].body.find("signature=wedged") != std::string::npos);
}

TEST(recovery_actor_soft_clears_idle) {
  std::filesystem::remove_all("/tmp/bus_test_policy_soft");
  setenv("CLAUDE_BUS_AUTO_CLEAR_MIN", "1", 1);      // idle_clear = 60 s
  setenv("CLAUDE_BUS_WEDGE_BUDGET_MS", "1000", 1);  // keep R4 off here
  const std::int64_t now = 2'000'000;
  RecoveryActor actor{"/tmp/bus_test_policy_soft", "soft"};

  // alice: 2 min idle at a ready Stop prompt, no mail, no in-flight → R1 clear.
  AgentInfo idle = mkInfo("Stop", now - 120'000);

  policy::PolicyContext ctx;
  ctx.now_wall_ms = now;
  ctx.now_mono_ms = now;
  ctx.pane = normalPane();
  policy::AgentSnapshot s;
  s.name = "alice";
  s.info = &idle;
  s.axes.turn = TurnAxis::Ready;
  s.axes.process = ProcessAxis::Alive;
  s.transcript_age_ms = 100;  // fresh → R4 off
  ctx.agents.push_back(s);

  auto out = actor.evaluate(ctx);
  // R1 acted: a /clear to commands-alice (deliver_when=idle) + an audit recover
  // row, and NO would-recover (the act superseded the observe log).
  bool saw_clear = false, saw_audit = false, saw_would = false;
  for (const auto& a : out) {
    if (a.topic == "commands-alice" && a.body == "/clear") {
      saw_clear = true;
      CHECK_EQ(a.deliver_when, 1);
    }
    if (a.topic == "audit" && a.protocol == "recover") saw_audit = true;
    if (a.protocol == "would-recover") saw_would = true;
  }
  CHECK(saw_clear);
  CHECK(saw_audit);
  CHECK(!saw_would);
}
