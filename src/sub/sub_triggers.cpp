// `bus triggers [AGENT...]` — the P3 context-management trigger feed.
//
// Two axes per agent, because ctx-fill ALONE is the wrong trigger:
// intervening (compact/handoff) mid-turn drops the agent's planned tool
// calls (the mid-stream-dropped-turn failure). So a context-manager must
// act only when it's URGENT *and* SAFE. The derivation lives in
// trigger_feed.{h,cpp} (shared with the `bus monitor` alarm-zone); this
// command is the CLI surface: derive → write every trigger file → render
// the ACT/WAIT/OK table. The always-running `bus monitor` loop refreshes
// the same files each tick, so a manual poll is rarely needed.

#include "../agent_status.h"
#include "../broker.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../sub.h"
#include "../trigger_feed.h"

#include <algorithm>
#include <map>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

using namespace bus::ansi;

// One "state" RPC → the broker's per-agent live snapshot. Empty object if
// the broker is unreachable (no triggers rather than a crash).
auto fetchState() -> json::Value {
  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("state")});
  auto resp =
      rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
  if (!resp || !resp->getOrBool("ok")) return json::Value::fromObject({});
  const auto* st = resp->get("state");
  if (st == nullptr || !st->isObject()) return json::Value::fromObject({});
  return *st;
}

auto fmtAge(std::int64_t ms) -> std::string {
  if (ms < 0) return "—";
  const auto s = ms / 1000;
  if (s < 60) return std::to_string(s) + "s";
  if (s < 3600) return std::to_string(s / 60) + "m";
  return std::to_string(s / 3600) + "h";
}

}  // namespace

auto subTriggers(std::span<const char* const> args) -> int {
  std::set<std::string> only;
  for (const char* a : args) only.insert(a);

  const auto state = fetchState();
  if (!state.isObject() || state.asObject().empty()) {
    std::println("no agents (broker unreachable or empty fleet)");
    return 0;
  }

  const std::int64_t now = nowMs();
  std::vector<Trig> trigs;
  for (const auto& [name, entry] : state.asObject()) {
    if (!only.empty() && !only.contains(name)) continue;
    Trig t = deriveTrig(name, entry, now);
    writeTrigger(t, now);  // refresh $STATE/triggers/<agent>.json for P3
    trigs.push_back(std::move(t));
  }
  std::sort(trigs.begin(), trigs.end(), [](const Trig& a, const Trig& b) {
    return a.agent < b.agent;
  });

  std::println("{}{:<10} {:<8} {:<10} {:<5}{}", kBold, "AGENT", "FILL",
               "BOUNDARY", "REC", kReset);
  int act = 0;
  for (const auto& t : trigs) {
    const Rec r = recOf(t);
    std::string_view rc;
    std::string rs;
    switch (r) {
      case Rec::Act: rc = kBrightRed; rs = "ACT"; ++act; break;
      case Rec::Wait: rc = kYellow; rs = "WAIT"; break;
      case Rec::Ok: rc = kGreen; rs = "OK"; break;
    }
    const std::string fill = t.pct < 0 ? "—" : std::to_string(t.pct) + "%";
    std::string boundary{boundaryStr(t.boundary)};
    if (t.boundary == Boundary::Done && t.done_ms >= 0)
      boundary += "(" + fmtAge(now - t.done_ms) + ")";
    else if (t.boundary == Boundary::Idle && t.idle_ms >= 0)
      boundary += "(" + fmtAge(t.idle_ms) + ")";
    std::println("{:<10} {:<8} {:<10} {}{:<5}{}", t.agent, fill, boundary, rc,
                 rs, kReset);
  }

  std::println("");
  if (act > 0) {
    std::println("{}{} agent(s) ACT now — urgent + at a safe boundary{}",
                 kBrightRed, act, kReset);
  } else {
    std::println("{}no agent both urgent and at a boundary{}", kGreen, kReset);
  }
  return 0;
}

}  // namespace bus
