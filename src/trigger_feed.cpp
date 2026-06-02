// Shared P3 trigger derivation — see trigger_feed.h for the why.

#include "trigger_feed.h"

#include "state_paths.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace bus {

namespace {

// URGENCY threshold — flag ACT/WAIT candidates EARLY (P3 fires at 50-60%,
// not at the 75/90 the monitor's CTX% column colors): acting needs a safe
// boundary, so start surfacing candidates with runway before ~90%.
constexpr int kUrgentPct = 60;
// A $STATE/done stamp within this window counts as the strong "done"
// boundary; older idle is the weaker "idle" boundary.
constexpr std::int64_t kDoneBoundaryWindowMs = 10 * 60 * 1000;  // 10 min

struct CtxFill {
  int pct{-1};                            // -1 = unknown
  std::string source{"used_percentage"};  // which field pct came from
};

// ctx-fill from elodin's $STATE/status/<agent>.json (read-only). Prefers
// the finer top-level `context_tokens` / `context_window_size` pair (P2's
// raw numerator — un-rounded, so it doesn't suffer used_percentage's
// integer-% coarseness near the urgency threshold). Falls back to the
// integer `used_percentage` when context_tokens is absent (the pre-P2
// broker), so the old and new emit shapes both work. -1 if absent/garbage.
auto ctxFill(std::string_view agent) -> CtxFill {
  CtxFill out;
  const std::string path =
      stateRoot() + "/status/" + std::string{agent} + ".json";
  std::ifstream in{path};
  if (!in) return out;
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  const auto v = json::parse(content);
  if (!v) return out;
  const auto* cw = v->get("context_window");
  const auto tokens = v->getOrInt("context_tokens", -1);
  const auto window =
      cw != nullptr ? cw->getOrInt("context_window_size", -1) : -1;
  if (tokens >= 0 && window > 0) {
    auto pct = (tokens * 100) / window;
    out.pct = static_cast<int>(pct > 100 ? 100 : pct);
    out.source = "context_tokens";
    return out;
  }
  if (cw == nullptr) return out;
  auto pct = cw->getOrInt("used_percentage", -1);
  if (pct > 100) pct = 100;
  out.pct = static_cast<int>(pct);
  return out;
}

// ts (ms) of the agent's most recent `bus done` stamp; -1 if none.
auto lastDoneMs(std::string_view agent) -> std::int64_t {
  const std::string path =
      stateRoot() + "/done/" + std::string{agent} + ".jsonl";
  std::ifstream in{path};
  if (!in) return -1;
  std::string line, last;
  while (std::getline(in, line))
    if (!line.empty()) last = line;
  if (last.empty()) return -1;
  const auto v = json::parse(last);
  if (!v) return -1;
  return v->getOrInt("ts", -1);
}

}  // namespace

auto boundaryStr(Boundary b) -> std::string_view {
  switch (b) {
    case Boundary::Done: return "done";
    case Boundary::Idle: return "idle";
    case Boundary::None: return "none";
  }
  return "none";
}

auto recOf(const Trig& t) -> Rec {
  if (t.pct < kUrgentPct) return Rec::Ok;          // includes pct<0 unknown
  return t.boundary == Boundary::None ? Rec::Wait : Rec::Act;
}

auto deriveTrig(std::string_view agent, const json::Value& entry,
                std::int64_t now) -> Trig {
  Trig t;
  t.agent = agent;
  const auto fill = ctxFill(agent);
  t.pct = fill.pct;
  t.source = fill.source;
  const auto label = entry.getOrString("state");
  const auto age_ms = entry.getOrInt("age_ms", -1);
  t.done_ms = lastDoneMs(agent);
  if (label == "WORKING" || label == "COMPACTING") {
    t.boundary = Boundary::None;  // mid-turn: UNSAFE
    t.idle_ms = -1;
  } else if (t.done_ms >= 0 && (now - t.done_ms) < kDoneBoundaryWindowMs) {
    t.boundary = Boundary::Done;  // recent completion: strongest boundary
    t.idle_ms = age_ms;
  } else {
    t.boundary = Boundary::Idle;  // between turns, no pending tool calls
    t.idle_ms = age_ms;
  }
  return t;
}

auto writeTrigger(const Trig& t, std::int64_t now) -> void {
  std::map<std::string, json::Value> urgency;
  urgency.insert(
      {"ctx_fill_pct", json::Value::from(static_cast<std::int64_t>(t.pct))});
  urgency.insert({"source", json::Value::from(t.source)});

  std::map<std::string, json::Value> safety;
  safety.insert(
      {"boundary", json::Value::from(std::string{boundaryStr(t.boundary)})});
  if (t.done_ms >= 0)
    safety.insert({"done_stamp_ms", json::Value::from(t.done_ms)});
  if (t.idle_ms >= 0) safety.insert({"idle_ms", json::Value::from(t.idle_ms)});

  std::map<std::string, json::Value> rec;
  rec.insert({"agent", json::Value::from(t.agent)});
  rec.insert({"urgency", json::Value::fromObject(std::move(urgency))});
  rec.insert({"safety", json::Value::fromObject(std::move(safety))});
  rec.insert({"computed_at_ms", json::Value::from(now)});

  const std::string dir = stateRoot() + "/triggers";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = dir + "/" + t.agent + ".json";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out{tmp};
    if (!out) return;
    out << json::serialize(json::Value::fromObject(std::move(rec))) << '\n';
  }
  std::filesystem::rename(tmp, path, ec);
}

}  // namespace bus
