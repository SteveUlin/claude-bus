// `bus agents [--json] [--kind K] [--session S] [--role R] [NAME]`
// `bus introduce NAME`
//
// Both read $CLAUDE_BUS_STATE/agents/*.json — the registry written by
// settings/hooks/agent-register.sh on SessionStart / SessionEnd. The
// registry is a side-effect, not a coordination primitive: stale
// entries are tolerable, missing fields render as `(none)`.
//
// `agents` lists / filters. `introduce` adds the broker's live state
// view (via the same RPC `bus state` uses) and points at
// `bus events` for the rich recent-activity tail.

#include "../broker.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../sub.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bus {

namespace {

auto stateDir() -> std::filesystem::path {
  const char* env = std::getenv("CLAUDE_BUS_STATE");
  return env ? std::filesystem::path{env}
             : std::filesystem::path{"/tmp/claude-bus"};
}

auto agentsDir() -> std::filesystem::path { return stateDir() / "agents"; }

auto slurp(const std::filesystem::path& p) -> std::string {
  std::ifstream f{p};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

auto loadEntries() -> std::vector<json::Value> {
  std::vector<json::Value> out;
  const auto dir = agentsDir();
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return out;
  for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
    if (de.path().extension() != ".json") continue;
    auto parsed = json::parse(slurp(de.path()));
    if (!parsed) continue;
    if (!parsed->isObject()) continue;
    out.push_back(std::move(*parsed));
  }
  return out;
}

// Empty filter = match everything. Otherwise the field's string value
// must equal `want`; null/missing fields never match a non-empty filter.
auto matches(const json::Value& entry, std::string_view key,
             std::string_view want) -> bool {
  if (want.empty()) return true;
  const auto* v = entry.get(key);
  if (v == nullptr || !v->isString()) return false;
  return v->asString() == want;
}

auto loadEntry(std::string_view name)
    -> std::expected<json::Value, std::string> {
  const auto path = agentsDir() / (std::string{name} + ".json");
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    return std::unexpected("no registry entry for '" + std::string{name} +
                           "'");
  }
  auto parsed = json::parse(slurp(path));
  if (!parsed) return std::unexpected("parse error: " + parsed.error());
  return std::move(*parsed);
}

auto stringOrNone(const json::Value& e, std::string_view key) -> std::string {
  const auto* v = e.get(key);
  if (v == nullptr || !v->isString()) return "(none)";
  return v->asString();
}

auto printTable(const std::vector<json::Value>& entries) -> void {
  std::println("{:<18} {:<12} {:<8} {:<14} {:<24}", "NAME", "KIND", "ROLE",
               "SESSION", "PROJECT");
  for (const auto& e : entries) {
    std::println("{:<18} {:<12} {:<8} {:<14} {:<24}",
                 e.getOrString("name"), stringOrNone(e, "kind"),
                 stringOrNone(e, "role"), stringOrNone(e, "session"),
                 stringOrNone(e, "project"));
  }
}

auto printCard(const json::Value& e) -> void {
  std::println("name:      {}", e.getOrString("name"));
  std::println("kind:      {}", stringOrNone(e, "kind"));
  std::println("role:      {}", stringOrNone(e, "role"));
  std::println("session:   {}", stringOrNone(e, "session"));
  std::println("project:   {}", stringOrNone(e, "project"));
  std::println("workspace: {}", stringOrNone(e, "workspace"));
  std::println("pane_id:   {}", stringOrNone(e, "pane_id"));
  std::println("started:   {}", e.getOrInt("started_at"));
}

}  // namespace

auto subAgents(std::span<const char* const> args) -> int {
  bool json_out = false;
  std::string kind, session, role, name;
  for (std::size_t i = 0; i < args.size(); ++i) {
    std::string_view a{args[i]};
    if (a == "--json") {
      json_out = true;
    } else if (a == "--kind" && i + 1 < args.size()) {
      kind = args[++i];
    } else if (a == "--session" && i + 1 < args.size()) {
      session = args[++i];
    } else if (a == "--role" && i + 1 < args.size()) {
      role = args[++i];
    } else if (!a.starts_with("--") && name.empty()) {
      name = std::string{a};
    } else {
      std::println(stderr,
                   "usage: bus agents [--json] [--kind K] [--session S] "
                   "[--role R] [NAME]");
      return 2;
    }
  }

  auto entries = loadEntries();

  std::vector<json::Value> filtered;
  for (auto& e : entries) {
    if (!name.empty() && e.getOrString("name") != name) continue;
    if (!matches(e, "kind", kind)) continue;
    if (!matches(e, "session", session)) continue;
    if (!matches(e, "role", role)) continue;
    filtered.push_back(std::move(e));
  }

  if (json_out) {
    std::vector<json::Value> arr;
    for (auto& e : filtered) arr.push_back(std::move(e));
    std::println("{}",
                 json::serialize(json::Value::fromArray(std::move(arr))));
    return 0;
  }

  if (!name.empty()) {
    if (filtered.empty()) {
      std::println(stderr, "bus agents: no agent named '{}'", name);
      return 1;
    }
    printCard(filtered[0]);
    return 0;
  }

  if (filtered.empty()) {
    std::println("(no agents registered)");
    return 0;
  }
  printTable(filtered);
  return 0;
}

auto subIntroduce(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus introduce NAME");
    return 2;
  }
  const std::string name{args[0]};

  auto entry = loadEntry(name);
  if (!entry) {
    std::println(stderr, "bus introduce: {}", entry.error());
    return 1;
  }

  printCard(*entry);

  // Live state via the same RPC `bus state` uses.
  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("state")});
  req.insert({"agent", json::Value::from(name)});
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp || !resp->getOrBool("ok")) {
    std::println("state:     (broker unreachable)");
    std::println("");
    std::println("hint: bus events --agent {} --since 30m", name);
    return 0;
  }
  const auto* state = resp->get("state");
  if (state == nullptr || !state->isObject()) {
    std::println("state:     (no state)");
  } else {
    const auto* live = state->get(name);
    if (live == nullptr || !live->isObject()) {
      std::println("state:     (not seen by broker)");
    } else {
      std::string last = live->getOrString("last_event");
      const auto tool = live->getOrString("last_tool");
      if (!tool.empty()) last += ":" + tool;
      std::println("state:     {} (age {}ms)",
                   live->getOrString("state", "?"), live->getOrInt("age_ms"));
      if (!last.empty()) std::println("last:      {}", last);
      std::println("attached:  {}",
                   live->getOrBool("attached") ? "yes" : "no");
    }
  }

  std::println("");
  std::println("hint: bus events --agent {} --since 30m", name);
  return 0;
}

}  // namespace bus
