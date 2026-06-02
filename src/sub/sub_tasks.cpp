// `bus tasks [OWNER...]` — the fleet task model at a glance.
//
// CONVERGENCE over the existing stores (no 4th store): joins the durable
// $STATE/done completion claims with the $STATE/triggers owner-liveness
// overlay. Today that's terminal-only (Option A) — every task shown is
// done — because there is no store of OPEN/in-flight tasks yet; identity
// at dispatch (Option B) lights up the open/in_flight states and the DEPS
// column. The derivation lives in task_model.{h,cpp}, shared with the
// critical-path view to come. See docs/task-model.md.

#include "../agent_status.h"
#include "../sub.h"
#include "../task_model.h"

#include <print>
#include <set>
#include <span>
#include <string>

namespace bus {

namespace {

using namespace bus::ansi;

auto fmtAge(std::int64_t ms) -> std::string {
  if (ms < 0) return "—";
  const auto s = ms / 1000;
  if (s < 60) return std::to_string(s) + "s";
  if (s < 3600) return std::to_string(s / 60) + "m";
  if (s < 86400) return std::to_string(s / 3600) + "h";
  return std::to_string(s / 86400) + "d";
}

auto truncate(std::string s, std::size_t w) -> std::string {
  if (s.size() <= w) return s;
  s.resize(w - 1);
  s += "…";
  return s;
}

// Owner-liveness cell: boundary + ctx%, dimmed when no fresh overlay. Ties
// the task view to the P3 trigger feed — same glance shows who's free.
auto liveCell(const OwnerLive& l) -> std::string {
  if (!l.valid) return std::string{kDim} + "—" + std::string{kReset};
  std::string out{l.boundary};
  if (l.ctx_pct >= 0) out += " " + std::to_string(l.ctx_pct) + "%";
  return out;
}

}  // namespace

auto subTasks(std::span<const char* const> args) -> int {
  std::set<std::string> only;
  for (const char* a : args) only.insert(a);

  const auto tasks = readTasks(only);
  if (tasks.empty()) {
    std::println("no tasks yet — agents stamp completions with "
                 "`bus done \"<task>\" \"<artifact>\"`");
    return 0;
  }

  const std::int64_t now = nowMs();
  std::println("{}{:<10} {:<6} {:<42} {:<7} {}{}", kBold, "OWNER", "STATE",
               "TITLE", "DONE", "LIVE", kReset);
  for (const auto& t : tasks) {
    const std::string age =
        t.done_claim.ts >= 0 ? fmtAge(now - t.done_claim.ts) : "—";
    std::println("{:<10} {:<6} {:<42} {:<7} {}", t.owner, t.state,
                 truncate(t.title, 42), age, liveCell(t.owner_live));
  }
  std::println("");
  std::println("{}{} task(s) — terminal-only (Option A); open/in-flight + "
               "deps light up once dispatch mints identity{}",
               kDim, tasks.size(), kReset);
  return 0;
}

}  // namespace bus
