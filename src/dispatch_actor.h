#pragma once

// DispatchActor — the work-queue as a Policy actor (M2, the unification MVP;
// docs/work-queue-dispatch.md). It reads the work-queue head + idle agents from
// the snapshot and assigns the head task to one idle agent per tick, emitting an
// Enqueue{inbox-<agent>, body, consume_from=<queue>} — the loop delivers the
// task as ordinary inbox mail AND consumes the queue head (the claim). A
// coordination pattern is thus broker-side code that already ships; activation
// is just creating the work-queue topic, with no loader and no per-agent hooks.
//
// MVP scope: one assignment per tick (assign the single head to the first
// eligible idle agent); re-assignment is naturally gated by the agent's
// inbox-pending / in-flight snapshot bits, so no actor-side cooldown is needed.
// Batch dispatch + load-balancing + re-queue-on-death are documented follow-ons.

#include <string_view>
#include <vector>

#include "policy.h"

namespace bus::delivery {

class DispatchActor : public policy::PolicyActor {
 public:
  auto name() const -> std::string_view override { return "dispatch"; }
  auto evaluate(const policy::PolicyContext& ctx)
      -> std::vector<policy::PolicyAction> override;
};

}  // namespace bus::delivery
