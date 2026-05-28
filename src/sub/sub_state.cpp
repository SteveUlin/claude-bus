// `bus state [AGENT]` — query the broker's view of agent lifecycle.

#include "../broker.h"
#include "../bus.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../sub.h"

#include <cstdio>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace bus {

auto subInflight(std::span<const char* const> args) -> int {
  if (!args.empty()) {
    std::println(stderr, "usage: bus inflight");
    return 2;
  }
  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("inflight")});
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus inflight: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus inflight: {}", resp->getOrString("error"));
    return 1;
  }
  const auto* arr = resp->get("in_flight");
  if (arr == nullptr || !arr->isArray() || arr->asArray().empty()) {
    std::println("(no in-flight deliveries)");
    return 0;
  }
  std::println("{:<32} {:<22} {:<12} {:>14}", "MSG_ID", "TOPIC", "AGENT",
               "DISPATCHED");
  for (const auto& f : arr->asArray()) {
    std::println("{:<32} {:<22} {:<12} {:>14}", f.getOrString("msg_id"),
                 f.getOrString("topic"), f.getOrString("agent"),
                 f.getOrInt("dispatched_at_ms"));
  }
  return 0;
}

auto subState(std::span<const char* const> args) -> int {
  std::string filter;
  bool show_all = false;
  for (const auto* a : args) {
    const std::string_view sv{a};
    if (sv == "--all") {
      show_all = true;
    } else if (sv.starts_with("--")) {
      std::println(stderr, "bus state: unknown flag \"{}\"", sv);
      return 2;
    } else if (filter.empty()) {
      filter = sv;
    } else {
      std::println(stderr, "usage: bus state [--all] [AGENT]");
      return 2;
    }
  }

  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("state")});
  if (!filter.empty()) {
    req.insert({"agent", json::Value::from(filter)});
  }
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus state: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus state: {}", resp->getOrString("error"));
    return 1;
  }
  const auto* state = resp->get("state");
  if (state == nullptr || !state->isObject()) return 0;

  std::println("{:<14} {:<10} {:<22} {:>8} {:<8} {}", "AGENT", "STATE",
               "LAST EVENT", "AGE_MS", "ATTACH", "PANE");
  for (const auto& [name, entry] : state->asObject()) {
    if (!entry.isObject()) continue;
    const auto state_label = entry.getOrString("state");
    // Hide tombstones by default. They pile up (one per old test
    // marker, dead session, etc.) and drown the live agents. `--all`
    // brings them back when you genuinely want the full history.
    // Single-agent queries always render — the user asked for that
    // name specifically.
    if (!show_all && filter.empty() &&
        (state_label == "GONE" || state_label == "ENDED")) {
      continue;
    }
    std::string last_event = entry.getOrString("last_event");
    const auto tool = entry.getOrString("last_tool");
    if (!tool.empty()) last_event += ":" + tool;
    if (last_event.empty()) last_event = "—";
    std::println("{:<14} {:<10} {:<22} {:>8} {:<8} {}", name,
                 state_label, last_event,
                 entry.getOrInt("age_ms"),
                 entry.getOrBool("attached") ? "yes" : "no",
                 entry.getOrBool("pane_exists") ? "yes" : "no");
  }
  return 0;
}

}  // namespace bus
