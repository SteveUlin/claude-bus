// `bus queue push/pop/peek/list` — ergonomic sugar over work-queue RPCs.
// Every action is one existing broker RPC; no broker-side changes.

#include "../broker.h"
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

// Ensure a work-queue topic exists.  Calls topic_create; treats
// "already exists" as success so `push` is idempotent on the queue name.
auto ensureQueue(const BrokerConfig& cfg, const std::string& name) -> bool {
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("topic_create")});
  req.insert({"name", json::Value::from(name)});
  req.insert({"kind", json::Value::from("work-queue")});
  auto resp = rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus queue push: {}", resp.error().message);
    return false;
  }
  if (!resp->getOrBool("ok")) {
    const auto err = resp->getOrString("error");
    // Idempotent: a pre-existing queue with any kind is fine.
    if (err.find("already exists") != std::string::npos) return true;
    std::println(stderr, "bus queue push: {}", err);
    return false;
  }
  return true;
}

}  // namespace

auto subQueue(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr, "usage: bus queue <push|pop|peek|list> [...]");
    return 2;
  }
  const std::string_view verb{args[0]};
  const auto sub = args.subspan(1);

  // ── push ────────────────────────────────────────────────────────────────
  // bus queue push <queue> <body...> [--protocol TAG]
  if (verb == "push") {
    if (sub.size() < 2) {
      std::println(stderr,
                   "usage: bus queue push QUEUE BODY [--protocol TAG]");
      return 2;
    }
    const std::string queue{sub[0]};
    std::string protocol = "text";

    // Collect body words (everything before a flag) and parse flags.
    std::string body{sub[1]};
    for (std::size_t i = 2; i < sub.size(); ++i) {
      const std::string_view a{sub[i]};
      if (a == "--protocol") {
        if (++i >= sub.size()) return 2;
        protocol = sub[i];
      } else if (a.starts_with("--")) {
        std::println(stderr, "bus queue push: unknown flag \"{}\"", a);
        return 2;
      } else {
        body += ' ';
        body += a;
      }
    }

    const auto cfg = resolveConfig();
    if (!ensureQueue(cfg, queue)) return 1;

    std::map<std::string, json::Value> req;
    req.insert({"op", json::Value::from("enqueue")});
    req.insert({"topic", json::Value::from(queue)});
    req.insert({"body", json::Value::from(body)});
    req.insert({"sender", json::Value::from("cli")});
    req.insert({"protocol", json::Value::from(protocol)});
    req.insert({"deliver_when", json::Value::from("immediate")});
    req.insert({"ttl_ms", json::Value::from(std::int64_t{0})});
    auto resp =
        rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
    if (!resp) {
      std::println(stderr, "bus queue push: {}", resp.error().message);
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus queue push: {}", resp->getOrString("error"));
      return 1;
    }
    std::println("queued → {} (id={})", queue, resp->getOrString("id"));
    return 0;
  }

  // ── pop ─────────────────────────────────────────────────────────────────
  // bus queue pop <queue> [--consumer ID]
  // Prints the popped body to stdout; returns 1 when the queue is empty.
  if (verb == "pop") {
    if (sub.empty()) {
      std::println(stderr, "usage: bus queue pop QUEUE [--consumer ID]");
      return 2;
    }
    const std::string queue{sub[0]};
    std::string consumer;
    for (std::size_t i = 1; i < sub.size(); ++i) {
      const std::string_view a{sub[i]};
      if (a == "--consumer") {
        if (++i >= sub.size()) return 2;
        consumer = sub[i];
      } else {
        std::println(stderr, "bus queue pop: unknown flag \"{}\"", a);
        return 2;
      }
    }

    const auto cfg = resolveConfig();
    std::map<std::string, json::Value> req;
    req.insert({"op", json::Value::from("fetch")});
    req.insert({"topic", json::Value::from(queue)});
    if (!consumer.empty()) req.insert({"consumer", json::Value::from(consumer)});
    auto resp =
        rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
    if (!resp) {
      std::println(stderr, "bus queue pop: {}", resp.error().message);
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus queue pop: {}", resp->getOrString("error"));
      return 1;
    }
    const auto* m = resp->get("message");
    if (m == nullptr || m->isNull()) return 1;  // empty — print nothing
    std::println("{}", m->getOrString("body"));
    return 0;
  }

  // ── peek ─────────────────────────────────────────────────────────────────
  // bus queue peek <queue>
  // Shows the head record body without consuming it.
  if (verb == "peek") {
    if (sub.empty()) {
      std::println(stderr, "usage: bus queue peek QUEUE");
      return 2;
    }
    const std::string queue{sub[0]};

    const auto cfg = resolveConfig();
    std::map<std::string, json::Value> req;
    req.insert({"op", json::Value::from("peek")});
    req.insert({"topic", json::Value::from(queue)});
    auto resp =
        rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
    if (!resp) {
      std::println(stderr, "bus queue peek: {}", resp.error().message);
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus queue peek: {}", resp->getOrString("error"));
      return 1;
    }
    const auto* msgs = resp->get("messages");
    if (msgs == nullptr || !msgs->isArray() || msgs->asArray().empty()) {
      std::println("(queue empty)");
      return 0;
    }
    // Show only the head record's body.
    std::println("{}", msgs->asArray()[0].getOrString("body"));
    return 0;
  }

  // ── list ─────────────────────────────────────────────────────────────────
  // bus queue list
  // Filters topic_list to kind==work-queue and prints each queue name.
  if (verb == "list") {
    const auto cfg = resolveConfig();
    std::map<std::string, json::Value> req;
    req.insert({"op", json::Value::from("topic_list")});
    auto resp =
        rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
    if (!resp) {
      std::println(stderr, "bus queue list: {}", resp.error().message);
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus queue list: {}", resp->getOrString("error"));
      return 1;
    }
    const auto* topics = resp->get("topics");
    if (topics == nullptr || !topics->isArray()) return 0;
    for (const auto& t : topics->asArray()) {
      if (t.getOrString("kind") != "work-queue") continue;
      std::println("{}", t.getOrString("name"));
    }
    return 0;
  }

  std::println(stderr, "bus queue: unknown command \"{}\"", verb);
  return 2;
}

}  // namespace bus
