// Fleet task model — pure reader. See task_model.h for the why.

#include "task_model.h"

#include "agent_status.h"  // nowMs
#include "json_min.h"
#include "state_paths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace bus {

namespace {

// An owner overlay older than this is treated as no-overlay. The trigger
// writer (the bus monitor 1 Hz loop) refreshes well inside it; a dead
// writer drops the overlay within the window rather than showing stale
// liveness. Mirrors the trigger feed's staleness contract.
constexpr std::int64_t kOwnerLiveStaleMs = 30 * 1000;

// Join $STATE/triggers/<agent>.json as the owner-liveness overlay. Invalid
// (valid=false) when absent, unparseable, or stale.
auto ownerLiveFor(std::string_view agent, std::int64_t now) -> OwnerLive {
  OwnerLive out;
  const std::string path =
      stateRoot() + "/triggers/" + std::string{agent} + ".json";
  std::ifstream in{path};
  if (!in) return out;
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  const auto v = json::parse(content);
  if (!v) return out;
  const auto computed = v->getOrInt("computed_at_ms", -1);
  if (computed < 0 || (now - computed) > kOwnerLiveStaleMs) return out;
  if (const auto* safety = v->get("safety"); safety != nullptr)
    out.boundary = safety->getOrString("boundary");
  if (const auto* urgency = v->get("urgency"); urgency != nullptr)
    out.ctx_pct = static_cast<int>(urgency->getOrInt("ctx_fill_pct", -1));
  out.valid = true;
  return out;
}

}  // namespace

auto readTasks(const std::set<std::string>& only) -> std::vector<Task> {
  std::vector<Task> tasks;
  const std::int64_t now = nowMs();

  // Source: $STATE/done/<agent>.jsonl — terminal completion claims (the
  // Option-A spine). Option B adds open/in-flight tasks from the work-queue
  // ahead of this loop; the schema already carries their states.
  const std::string dir = stateRoot() + "/done";
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return tasks;
  for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".jsonl") continue;
    std::ifstream in{entry.path()};
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      const auto v = json::parse(line);
      if (!v) continue;
      const std::string owner = v->getOrString("agent");
      if (!only.empty() && !only.contains(owner)) continue;
      Task t;
      t.owner = owner;
      t.title = v->getOrString("task");
      t.done_claim.artifact = v->getOrString("claimed_artifact");
      t.done_claim.ts = v->getOrInt("ts", -1);
      t.id = owner + "-" + std::to_string(t.done_claim.ts);
      t.state = "done";  // Option A: every task here is terminal
      tasks.push_back(std::move(t));
    }
  }

  // Newest-first by completion ts.
  std::ranges::sort(tasks, [](const Task& a, const Task& b) {
    return a.done_claim.ts > b.done_claim.ts;
  });

  // Overlay owner liveness once per owner (cache to avoid re-reading a
  // trigger file per task).
  std::set<std::string> seen;
  std::vector<std::pair<std::string, OwnerLive>> cache;
  for (auto& t : tasks) {
    auto it = std::ranges::find(cache, t.owner, &std::pair<std::string, OwnerLive>::first);
    if (it == cache.end()) {
      cache.emplace_back(t.owner, ownerLiveFor(t.owner, now));
      it = std::prev(cache.end());
    }
    t.owner_live = it->second;
  }

  return tasks;
}

}  // namespace bus
