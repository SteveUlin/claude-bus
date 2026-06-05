#include "policy.h"

#include <iterator>
#include <utility>

namespace bus::policy {

auto PolicyEngine::registerActor(std::unique_ptr<PolicyActor> actor) -> void {
  if (actor) actors_.push_back(std::move(actor));
}

auto PolicyEngine::evaluate(const PolicyContext& ctx)
    -> std::vector<PolicyAction> {
  std::vector<PolicyAction> out;
  for (const auto& actor : actors_) {
    auto actions = actor->evaluate(ctx);
    out.insert(out.end(), std::make_move_iterator(actions.begin()),
               std::make_move_iterator(actions.end()));
  }
  return out;
}

auto PolicyEngine::pruneDeadAgents(const std::set<std::string>& live) -> void {
  for (const auto& actor : actors_) actor->pruneDeadAgents(live);
}

}  // namespace bus::policy
