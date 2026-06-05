#include "dispatch_actor.h"

#include "agent_status.h"

namespace bus::delivery {

auto DispatchActor::evaluate(const policy::PolicyContext& ctx)
    -> std::vector<policy::PolicyAction> {
  std::vector<policy::PolicyAction> out;
  if (ctx.queue_head.empty()) return out;  // no work → inert (the default)

  // First eligible idle agent: ready at the prompt, no human attached, not mid
  // blocking-op, and not already holding queued / in-flight work. The
  // inbox-pending + in-flight bits are what gate re-assignment — after this
  // agent receives the task its inbox goes pending, so it drops out next tick
  // until it drains, spreading tasks across idle agents with no actor cooldown.
  const policy::AgentSnapshot* target = nullptr;
  for (const auto& s : ctx.agents) {
    if (s.axes.turn != TurnAxis::Ready) continue;
    if (s.attached || s.blocking_op) continue;
    if (s.inbox_pending || s.has_in_flight) continue;
    target = &s;
    break;
  }
  if (target == nullptr) return out;  // queue has work but no free agent

  // Assign the head task: deliver it as inbox mail AND consume the queue head.
  // The loop performs both (the consume reuses the fetch primitive); the actor
  // only expresses intent — it advances no cursor (§1.4).
  const auto& task = ctx.queue_head.front();
  policy::PolicyAction a;
  a.kind = policy::PolicyAction::Kind::Enqueue;
  a.agent = target->name;
  a.topic = "inbox-" + target->name;
  a.body = task.body;
  a.protocol = "work-assignment";
  a.consume_from = task.topic;
  out.push_back(std::move(a));
  return out;
}

}  // namespace bus::delivery
