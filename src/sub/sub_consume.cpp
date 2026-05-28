// `bus msg fetch TOPIC` / `bus msg peek TOPIC` / `bus msg body MSG_ID` — consume /
// inspect topic records via broker RPC.

#include "../broker.h"
#include "../bus.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../sub.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace bus {

namespace {

auto printMessage(const json::Value& m) -> void {
  if (!m.isObject()) return;
  std::println("=== {} ===", m.getOrString("id"));
  std::println("sender:       {}", m.getOrString("sender"));
  std::println("protocol:     {}", m.getOrString("protocol"));
  std::println("ttl_ms:       {}", m.getOrInt("ttl_ms"));
  std::println("deliver_when: {}", m.getOrInt("deliver_when") == 1
                                       ? "idle"
                                       : "immediate");
  std::println("body:");
  std::string indented = "| ";
  const auto& b = m.getOrString("body");
  for (char c : b) {
    indented += c;
    if (c == '\n') indented += "| ";
  }
  std::println("{}", indented);
}

}  // namespace

auto subFetch(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr,
                 "usage: bus msg fetch TOPIC [--consumer ID]");
    return 2;
  }
  const std::string topic{args[0]};
  std::string consumer;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--consumer") {
      if (++i >= args.size()) return 2;
      consumer = args[i];
    } else {
      std::println(stderr, "bus msg fetch: unknown flag \"{}\"", a);
      return 2;
    }
  }

  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("fetch")});
  req.insert({"topic", json::Value::from(topic)});
  if (!consumer.empty()) {
    req.insert({"consumer", json::Value::from(consumer)});
  }
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus msg fetch: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus msg fetch: {}", resp->getOrString("error"));
    return 1;
  }
  const auto* m = resp->get("message");
  if (m == nullptr || m->isNull()) {
    // Nothing to fetch — clean exit so callers can `while bus msg fetch …`.
    return 0;
  }
  printMessage(*m);
  return 0;
}

auto subPeek(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr,
                 "usage: bus msg peek TOPIC [--consumer ID] [--limit N]");
    return 2;
  }
  const std::string topic{args[0]};
  std::string consumer;
  std::int64_t limit = 0;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--consumer") {
      if (++i >= args.size()) return 2;
      consumer = args[i];
    } else if (a == "--limit") {
      if (++i >= args.size()) return 2;
      limit = std::atoll(args[i]);
    } else {
      std::println(stderr, "bus msg peek: unknown flag \"{}\"", a);
      return 2;
    }
  }

  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("peek")});
  req.insert({"topic", json::Value::from(topic)});
  if (!consumer.empty()) {
    req.insert({"consumer", json::Value::from(consumer)});
  }
  if (limit > 0) req.insert({"limit", json::Value::from(limit)});

  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus msg peek: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus msg peek: {}", resp->getOrString("error"));
    return 1;
  }
  if (const auto* msgs = resp->get("messages");
      msgs != nullptr && msgs->isArray()) {
    for (const auto& m : msgs->asArray()) printMessage(m);
  }
  return 0;
}

auto subBody(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr, "usage: bus msg body MSG_ID");
    return 2;
  }
  const std::string msg_id{args[0]};

  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("body")});
  req.insert({"msg_id", json::Value::from(msg_id)});
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus msg body: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus msg body: {}", resp->getOrString("error"));
    return 1;
  }
  const auto* m = resp->get("message");
  if (m == nullptr || !m->isObject()) {
    std::println(stderr, "bus msg body: malformed response");
    return 1;
  }
  std::fputs(m->getOrString("body").c_str(), stdout);
  return 0;
}

}  // namespace bus
