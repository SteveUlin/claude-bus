#include "blackboard_actor.h"

#include <format>

namespace bus::delivery {

auto BlackboardActor::evaluate(const policy::PolicyContext& ctx)
    -> std::vector<policy::PolicyAction> {
  std::vector<policy::PolicyAction> out;
  // Fan out each new board post to every live agent except its author. Pure
  // Enqueue; the loop already consumed each post into board_updates (advancing
  // the _dispatch cursor), so this actor holds no state.
  for (const auto& upd : ctx.board_updates) {
    for (const auto& s : ctx.agents) {
      if (s.name == upd.author) continue;  // don't notify the poster
      policy::PolicyAction a;
      a.kind = policy::PolicyAction::Kind::Enqueue;
      a.agent = s.name;
      a.topic = "inbox-" + s.name;
      a.protocol = "blackboard-notify";
      a.body = std::format("blackboard '{}' updated by {}: {}", upd.topic,
                           upd.author, upd.body);
      out.push_back(std::move(a));
    }
  }
  return out;
}

}  // namespace bus::delivery
