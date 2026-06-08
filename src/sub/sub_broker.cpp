// `bus broker <run|info>` — broker daemon entrypoint + introspection.
//
// `run` lives in a foreground pane (typically the ops tab's floating
// pane); closing the pane ends the bus. No daemonization, no PID
// tracking. `runBroker` holds a flock on broker.pid so two `run`
// invocations can't both serve (the second exits 1).
//
// `info` queries the live broker's `info` RPC — pid, uptime, and the
// build_commit the running binary was compiled from. The latter answers
// "is the live broker canonical?" (gap #11) over one RPC, replacing
// /proc/<pid>/exe forensics on a possibly-deleted binary path.

#include "../broker.h"
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

namespace {

auto brokerInfo() -> int {
  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> m;
  m.insert({"op", json::Value::from("info")});
  auto resp = rpc::call(cfg.socket_path, json::Value::fromObject(std::move(m)));
  if (!resp) {
    std::println(stderr, "bus broker info: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus broker info: {}", resp->getOrString("error"));
    return 1;
  }
  std::println("pid          {}", resp->getOrInt("pid", -1));
  std::println("uptime_ms    {}", resp->getOrInt("uptime_ms", -1));
  std::println("state_dir    {}", resp->getOrString("state_dir"));
  std::println("topic_count  {}", resp->getOrInt("topic_count", -1));
  std::println("build_commit {}", resp->getOrString("build_commit"));
  return 0;
}

auto brokerGc() -> int {
  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> m;
  m.insert({"op", json::Value::from("gc")});
  auto resp = rpc::call(cfg.socket_path, json::Value::fromObject(std::move(m)));
  if (!resp) {
    std::println(stderr, "bus broker gc: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus broker gc: {}", resp->getOrString("error"));
    return 1;
  }
  const auto* reaped = resp->get("reaped");
  const auto* skipped = resp->get("skipped");
  const auto n_reaped = (reaped && reaped->isArray()) ? reaped->asArray().size() : 0;
  if (reaped && reaped->isArray()) {
    for (const auto& t : reaped->asArray()) {
      if (t.isString()) std::println("reaped  {}", t.asString());
    }
  }
  if (skipped && skipped->isArray()) {
    for (const auto& s : skipped->asArray()) {
      std::println("skipped {:<24} ({})", s.getOrString("name"),
                   s.getOrString("reason"));
    }
  }
  std::println("--- {} reaped, {} skipped", n_reaped,
               (skipped && skipped->isArray()) ? skipped->asArray().size() : 0);
  return 0;
}

}  // namespace

auto subBroker(std::span<const char* const> args) -> int {
  const std::string_view verb = args.empty() ? "" : std::string_view{args[0]};
  if (args.size() == 1 && verb == "run") return runBroker(resolveConfig());
  if (args.size() == 1 && verb == "info") return brokerInfo();
  if (args.size() == 1 && verb == "gc") return brokerGc();
  std::println(stderr, "usage: bus broker <run|info|gc>");
  return 2;
}

}  // namespace bus
