// `bus topic create / list / show` — registry management via broker RPC.

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
#include <vector>

namespace bus {

namespace {

// Print one TopicConfig (already JSON) in a human format.
auto printTopic(const json::Value& v) -> void {
  if (!v.isObject()) {
    std::println("(non-object topic)");
    return;
  }
  std::println("name:             {}", v.getOrString("name"));
  std::println("kind:             {}", v.getOrString("kind"));
  std::println("max_record_bytes: {}", v.getOrInt("max_record_bytes"));
  std::println("retention_ms:     {}", v.getOrInt("retention_ms"));
  if (const auto* kc = v.get("kind_config"); kc != nullptr && !kc->isNull()) {
    std::println("kind_config:      {}", json::serialize(*kc));
  }
}

}  // namespace

auto subTopic(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr,
                 "usage: bus topic <create|list|show> [...]");
    return 2;
  }
  const std::string_view verb{args[0]};
  const auto cfg = resolveConfig();

  if (verb == "list") {
    std::map<std::string, json::Value> m;
    m.insert({"op", json::Value::from("topic_list")});
    auto resp = rpc::call(cfg.socket_path,
                          json::Value::fromObject(std::move(m)));
    if (!resp) {
      std::println(stderr, "bus topic list: {}", resp.error());
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus topic list: {}", resp->getOrString("error"));
      return 1;
    }
    const auto* topics = resp->get("topics");
    if (topics == nullptr || !topics->isArray()) {
      return 0;  // empty
    }
    for (const auto& t : topics->asArray()) {
      std::println("{:<32} {:<16} kind_config={}",
                   t.getOrString("name"),
                   t.getOrString("kind"),
                   t.get("kind_config")
                       ? json::serialize(*t.get("kind_config"))
                       : "(none)");
    }
    return 0;
  }

  if (verb == "show") {
    if (args.size() < 2) {
      std::println(stderr, "usage: bus topic show NAME");
      return 2;
    }
    std::map<std::string, json::Value> m;
    m.insert({"op", json::Value::from("topic_show")});
    m.insert({"name", json::Value::from(std::string{args[1]})});
    auto resp = rpc::call(cfg.socket_path,
                          json::Value::fromObject(std::move(m)));
    if (!resp) {
      std::println(stderr, "bus topic show: {}", resp.error());
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus topic show: {}", resp->getOrString("error"));
      return 1;
    }
    if (const auto* t = resp->get("topic"); t != nullptr) {
      printTopic(*t);
    }
    return 0;
  }

  if (verb == "create") {
    // bus topic create NAME --kind KIND [--max-bytes N] [--retention-ms N]
    if (args.size() < 2) {
      std::println(stderr,
                   "usage: bus topic create NAME [--kind KIND] "
                   "[--max-bytes N] [--retention-ms MS]");
      return 2;
    }
    std::string name{args[1]};
    std::string kind = "work-queue";  // sensible default
    std::int64_t max_bytes = 4096;
    std::int64_t retention = 0;
    std::string subscribers;  // comma-separated for pubsub kind

    for (std::size_t i = 2; i < args.size(); ++i) {
      const std::string_view a{args[i]};
      if (a == "--kind") {
        if (++i >= args.size()) return 2;
        kind = args[i];
      } else if (a == "--max-bytes") {
        if (++i >= args.size()) return 2;
        max_bytes = std::atoll(args[i]);
      } else if (a == "--retention-ms") {
        if (++i >= args.size()) return 2;
        retention = std::atoll(args[i]);
      } else if (a == "--subscribers") {
        if (++i >= args.size()) return 2;
        subscribers = args[i];
      } else {
        std::println(stderr, "bus topic create: unknown flag \"{}\"", a);
        return 2;
      }
    }

    std::map<std::string, json::Value> m;
    m.insert({"op", json::Value::from("topic_create")});
    m.insert({"name", json::Value::from(name)});
    m.insert({"kind", json::Value::from(kind)});
    m.insert({"max_record_bytes", json::Value::from(max_bytes)});
    m.insert({"retention_ms", json::Value::from(retention)});

    // Parse subscribers (pubsub kind) into kind_config.
    if (!subscribers.empty()) {
      std::vector<json::Value> subs;
      std::string cur;
      for (char c : subscribers) {
        if (c == ',') {
          if (!cur.empty()) subs.push_back(json::Value::from(cur));
          cur.clear();
        } else if (c != ' ' && c != '\t') {
          cur += c;
        }
      }
      if (!cur.empty()) subs.push_back(json::Value::from(cur));
      std::map<std::string, json::Value> kc;
      kc.insert({"subscribers", json::Value::fromArray(std::move(subs))});
      m.insert(
          {"kind_config", json::Value::fromObject(std::move(kc))});
    }
    auto resp = rpc::call(cfg.socket_path,
                          json::Value::fromObject(std::move(m)));
    if (!resp) {
      std::println(stderr, "bus topic create: {}", resp.error());
      return 1;
    }
    if (!resp->getOrBool("ok")) {
      std::println(stderr, "bus topic create: {}",
                   resp->getOrString("error"));
      return 1;
    }
    std::println("created {} (kind={})", name, kind);
    return 0;
  }

  std::println(stderr, "bus topic: unknown verb \"{}\"", verb);
  return 2;
}

}  // namespace bus
