// Fleet task model — pure reader. See task_model.h for the why.

#include "task_model.h"

#include "agent_status.h"  // nowMs
#include "envelope.h"
#include "journal.h"
#include "json_min.h"
#include "state_paths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

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
  const std::int64_t now = nowMs();

  // ── Spine: the `tasks` append-log topic (open / cancelled records) ──
  // Read-in-full via Journal::dump() — no cursor, no broker round-trip.
  // Keyed by id; last open-record wins on a re-open. `mint_ts` is the
  // sort key for not-yet-done tasks (done tasks sort by completion ts).
  std::map<std::string, Task> by_id;
  std::map<std::string, std::int64_t> mint_ts;
  std::set<std::string> cancelled;

  const std::string topic_path =
      stateRoot() + "/topics/" + std::string{kTasksTopic} + ".log";
  bus::Journal spine{topic_path};
  if (auto dumped = spine.dump()) {
    for (const auto& rec : *dumped) {
      const auto task_env = bus::msg::decodeEnvelope(rec.payload);
      const auto v = json::parse(task_env.body);
      if (!v) continue;
      const std::string id = v->getOrString("id");
      if (id.empty()) continue;
      if (v->getOrBool("cancelled", false)) {
        cancelled.insert(id);
        continue;
      }
      Task t;
      t.id = id;
      t.owner = v->getOrString("owner");
      t.title = v->getOrString("title");
      if (const auto* deps = v->get("deps"); deps != nullptr && deps->isArray())
        for (const auto& d : deps->asArray())
          if (d.isString()) t.deps.push_back(d.asString());
      by_id[id] = std::move(t);
      mint_ts[id] = v->getOrInt("ts", 0);
    }
  }

  // ── Terminal claims: $STATE/done/<agent>.jsonl ──
  // A claim with `task_id` closes a spine task (match by id). A claim
  // without one is a legacy A-style anonymous completion — surfaced as a
  // standalone terminal task so nothing is lost.
  std::vector<Task> anon_done;
  const std::string dir = stateRoot() + "/done";
  std::error_code ec;
  if (std::filesystem::exists(dir, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".jsonl") continue;
      std::ifstream in{entry.path()};
      std::string line;
      while (std::getline(in, line)) {
        if (line.empty()) continue;
        const auto v = json::parse(line);
        if (!v) continue;
        DoneClaim claim;
        claim.artifact = v->getOrString("claimed_artifact");
        claim.ts = v->getOrInt("ts", -1);
        const std::string task_id = v->getOrString("task_id");
        if (!task_id.empty()) {
          if (auto it = by_id.find(task_id); it != by_id.end()) {
            it->second.done_claim = claim;  // close a known spine task
          } else {
            // Claim references an id with no open record (broker GC'd the
            // spine, or done landed first). Still show it as terminal.
            Task t;
            t.id = task_id;
            t.owner = v->getOrString("agent");
            t.title = v->getOrString("task");
            t.done_claim = claim;
            by_id[task_id] = std::move(t);
          }
          continue;
        }
        Task t;
        t.owner = v->getOrString("agent");
        t.title = v->getOrString("task");
        t.done_claim = claim;
        t.id = t.owner + "-" + std::to_string(claim.ts);
        anon_done.push_back(std::move(t));
      }
    }
  }

  // ── State precedence per task: done-claim ⇒ done; else a cancel ⇒
  // cancelled; else owner mid-turn (liveness boundary=none) ⇒ in_flight;
  // else open. in_flight is INFERRED from owner liveness (auri's ruling):
  // it means "the owner is live", NOT "provably on THIS task".
  std::vector<std::pair<std::string, OwnerLive>> live_cache;
  auto liveFor = [&](const std::string& owner) -> const OwnerLive& {
    auto it = std::ranges::find(live_cache, owner,
                                &std::pair<std::string, OwnerLive>::first);
    if (it == live_cache.end()) {
      live_cache.emplace_back(owner, ownerLiveFor(owner, now));
      it = std::prev(live_cache.end());
    }
    return it->second;
  };

  std::vector<Task> tasks;
  tasks.reserve(by_id.size() + anon_done.size());
  for (auto& [id, t] : by_id) {
    t.owner_live = liveFor(t.owner);
    if (t.done_claim.ts >= 0)
      t.state = "done";
    else if (cancelled.contains(id))
      t.state = "cancelled";
    else if (t.owner_live.valid && t.owner_live.boundary == "none")
      t.state = "in_flight";
    else
      t.state = "open";
    tasks.push_back(std::move(t));
  }
  for (auto& t : anon_done) {
    t.owner_live = liveFor(t.owner);
    t.state = "done";
    tasks.push_back(std::move(t));
  }

  if (!only.empty())
    std::erase_if(tasks, [&](const Task& t) { return !only.contains(t.owner); });

  // Sort: live work first (in_flight, open), then done, then cancelled;
  // within a rank, newest first (mint ts for not-done, completion ts for
  // done) so the cockpit reads top-down by urgency then recency.
  auto rank = [](const Task& t) {
    if (t.state == "in_flight") return 0;
    if (t.state == "open") return 1;
    if (t.state == "done") return 2;
    return 3;  // cancelled / unknown
  };
  auto sortTs = [&](const Task& t) {
    return t.done_claim.ts >= 0 ? t.done_claim.ts
                                : (mint_ts.count(t.id) ? mint_ts[t.id] : 0);
  };
  std::ranges::sort(tasks, [&](const Task& a, const Task& b) {
    const int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return sortTs(a) > sortTs(b);
  });

  return tasks;
}

auto buildTaskGraph(const std::vector<Task>& tasks) -> TaskGraph {
  TaskGraph g;

  std::map<std::string, const Task*, std::less<>> by_id;
  for (const auto& t : tasks) by_id.emplace(t.id, &t);

  // Longest dependency chain by hops. depth(id) = 1 + max depth over deps;
  // pred[id] = the dep achieving that max. Memoized DFS; a node seen while
  // still in-progress is a cycle (flagged, excluded from the depth so the
  // recursion still terminates).
  std::map<std::string, int> depth;   // 0 = uncomputed/leaf-root
  std::map<std::string, std::string> pred;
  std::map<std::string, int> color;   // 0=unseen 1=in-progress 2=done
  auto computeDepth = [&](auto&& self, const std::string& id) -> int {
    auto byIt = by_id.find(id);
    if (byIt == by_id.end()) return 0;  // dangling — handled separately
    if (color[id] == 1) {               // back-edge → cycle
      g.has_cycle = true;
      return 0;
    }
    if (color[id] == 2) return depth[id];
    color[id] = 1;
    int best = 0;
    std::string best_dep;
    for (const auto& d : byIt->second->deps) {
      if (!by_id.contains(d)) continue;  // dangling dep adds no depth
      const int cand = self(self, d) + 1;
      if (cand > best) {
        best = cand;
        best_dep = d;
      }
    }
    color[id] = 2;
    depth[id] = best;
    if (!best_dep.empty()) pred[id] = best_dep;
    return best;
  };

  std::string deepest;
  int deepest_d = -1;
  for (const auto& t : tasks) {
    const int d = computeDepth(computeDepth, t.id);
    if (d > deepest_d) {
      deepest_d = d;
      deepest = t.id;
    }
  }
  // Reconstruct leaf→root via pred, then reverse to root→leaf. A cycle leaves
  // pred self-referential (pred[a]=b, pred[b]=a), so guard the walk against
  // revisiting a node — the chain is best-effort under a cycle anyway.
  if (deepest_d > 0) {
    std::set<std::string> walked;
    std::string cur = deepest;
    while (walked.insert(cur).second) {
      g.longest_chain.push_back(cur);
      auto it = pred.find(cur);
      if (it == pred.end()) break;
      cur = it->second;
    }
    std::ranges::reverse(g.longest_chain);
  }

  // Ready / blocked (open + in_flight only) + dangling deps. A task is ready
  // when every dep is present AND done; any missing-or-not-done dep blocks.
  for (const auto& t : tasks) {
    for (const auto& d : t.deps)
      if (!by_id.contains(d)) g.dangling.push_back({t.id, d});

    if (t.state != "open" && t.state != "in_flight") continue;
    bool deps_met = true;
    for (const auto& d : t.deps) {
      auto it = by_id.find(d);
      if (it == by_id.end() || it->second->state != "done") {
        deps_met = false;
        break;
      }
    }
    if (!deps_met) {
      g.blocked.push_back(t.id);  // open OR in_flight, waiting on a dep
    } else if (t.state == "open") {
      g.ready.push_back(t.id);  // not started + deps satisfied = actionable
    }
    // in_flight + deps_met → already running; shown in the chain, no bucket.
  }

  return g;
}

}  // namespace bus
