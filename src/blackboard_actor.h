#pragma once

// BlackboardActor — a blackboard (shared notes) as a Policy actor: the second
// coordination pattern, proving the substrate admits FAN-OUT (1→N broadcast),
// the complement to the work-queue's competing-consumer claim (M2). On a new
// post to a blackboard topic it notifies every live agent except the author,
// delivered as ordinary inbox mail.
//
// The PUREST actor: STATELESS. The "have I notified this post" watermark is the
// loop's _dispatch cursor (advanced when the loop folds a post into the
// snapshot — consume-on-read), so the actor is a pure function of the snapshot.
// Pure Enqueue — no consume_from, no new action kind. See
// docs/blackboard-notify.md.

#include <string_view>
#include <vector>

#include "policy.h"

namespace bus::delivery {

class BlackboardActor : public policy::PolicyActor {
 public:
  auto name() const -> std::string_view override { return "blackboard"; }
  auto evaluate(const policy::PolicyContext& ctx)
      -> std::vector<policy::PolicyAction> override;
};

}  // namespace bus::delivery
