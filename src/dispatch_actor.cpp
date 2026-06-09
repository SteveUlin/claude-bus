#include "dispatch_actor.h"

#include <algorithm>
#include <vector>

#include "agent_status.h"

namespace bus::delivery {

auto DispatchActor::evaluate(const policy::PolicyContext& ctx)
    -> std::vector<policy::PolicyAction> {
  std::vector<policy::PolicyAction> out;
  if (ctx.queue_head.empty()) return out;  // no work → inert (the default)

  // Batch-assign: for each eligible idle agent find the first unclaimed task
  // it is allowed to take. "Eligible" means: workers is empty (pool-wide) OR
  // the agent's name appears in workers. One task per agent per tick; FIFO
  // within eligibility. The claimed[] bitset prevents the same task from
  // being assigned twice in a single tick.
  std::vector<bool> claimed(ctx.queue_head.size(), false);

  for (const auto& s : ctx.agents) {
    if (s.axes.turn != TurnAxis::Ready) continue;
    if (s.attached || s.blocking_op) continue;
    if (s.inbox_pending || s.has_in_flight) continue;

    // Find the first unclaimed task this agent is eligible for.
    for (std::size_t i = 0; i < ctx.queue_head.size(); ++i) {
      if (claimed[i]) continue;
      const auto& task = ctx.queue_head[i];
      // Empty workers list = pool-wide; otherwise agent must be listed.
      if (!task.workers.empty() &&
          std::find(task.workers.begin(), task.workers.end(), s.name) ==
              task.workers.end()) {
        continue;
      }
      // Assign: deliver the task as inbox mail AND consume the queue head.
      // The loop performs both (the consume reuses the fetch primitive,
      // advancing the source cursor in this emission order); the actor
      // advances no cursor (§1.4) — it only expresses intent.
      policy::PolicyAction a;
      a.kind = policy::PolicyAction::Kind::Enqueue;
      a.agent = s.name;
      a.topic = "inbox-" + s.name;
      a.body = task.body;
      a.protocol = "work-assignment";
      a.consume_from = task.topic;
      out.push_back(std::move(a));
      claimed[i] = true;
      break;  // one task per agent per tick
    }
  }
  return out;
}

}  // namespace bus::delivery
