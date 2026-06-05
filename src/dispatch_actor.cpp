#include "dispatch_actor.h"

#include "agent_status.h"

namespace bus::delivery {

auto DispatchActor::evaluate(const policy::PolicyContext& ctx)
    -> std::vector<policy::PolicyAction> {
  std::vector<policy::PolicyAction> out;
  if (ctx.queue_head.empty()) return out;  // no work → inert (the default)

  // Batch-assign: pair queued tasks (FIFO, head order) with eligible idle
  // agents — ready at the prompt, no human attached, not mid blocking-op, not
  // already holding queued / in-flight work. One task per agent this tick, in
  // order, so the loop's in-order consume gives task[i] → agent[i]. The
  // inbox-pending bit then drops each agent out next tick until it drains, so
  // no agent is over-assigned even across ticks.
  std::size_t task_idx = 0;
  for (const auto& s : ctx.agents) {
    if (task_idx >= ctx.queue_head.size()) break;  // out of tasks
    if (s.axes.turn != TurnAxis::Ready) continue;
    if (s.attached || s.blocking_op) continue;
    if (s.inbox_pending || s.has_in_flight) continue;

    // Assign: deliver the task as inbox mail AND consume the queue head. The
    // loop performs both (the consume reuses the fetch primitive, advancing the
    // source cursor in this same emission order); the actor advances no cursor
    // (§1.4) — it only expresses intent.
    const auto& task = ctx.queue_head[task_idx];
    policy::PolicyAction a;
    a.kind = policy::PolicyAction::Kind::Enqueue;
    a.agent = s.name;
    a.topic = "inbox-" + s.name;
    a.body = task.body;
    a.protocol = "work-assignment";
    a.consume_from = task.topic;
    out.push_back(std::move(a));
    ++task_idx;
  }
  return out;
}

}  // namespace bus::delivery
