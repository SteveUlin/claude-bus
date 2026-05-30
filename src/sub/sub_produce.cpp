// `bus msg enqueue TOPIC body [...]` — and sugar verbs `bus msg mail`, `bus msg slash`.

#include "../broker.h"
#include "../bus.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../state_paths.h"
#include "../sub.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

auto senderFromEnv() -> std::string {
  if (const char* env = std::getenv("CLAUDE_BUS_AGENT_ID")) return env;
  return "unknown";
}

// Default TTL for kinds whose records go stale fast in practice.
// 1 hour gives plenty of headroom for legitimate delays (broker
// restart, idle agent, busy queue) while keeping the long-tail of
// truly-stale mail self-pruning. Producers can override via --ttl.
// See docs/elodin-ideation.md §3 + docs/context-budget.md for the
// reasoning.
constexpr std::int64_t kDefaultMailTtlMs = 60 * 60 * 1000;    // 1 hour
constexpr std::int64_t kDefaultSlashTtlMs = 60 * 60 * 1000;   // 1 hour

// Common publish path: build a JSON request, send to broker, print
// the resulting record id on stdout.
auto callEnqueue(const std::string& topic, const std::string& body,
                 const std::string& protocol,
                 const std::string& deliver_when,
                 std::int64_t ttl_ms) -> int {
  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("enqueue")});
  req.insert({"topic", json::Value::from(topic)});
  req.insert({"body", json::Value::from(body)});
  req.insert({"sender", json::Value::from(senderFromEnv())});
  req.insert({"protocol", json::Value::from(protocol)});
  req.insert({"deliver_when", json::Value::from(deliver_when)});
  req.insert({"ttl_ms", json::Value::from(ttl_ms)});
  auto resp =
      rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus msg enqueue: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus msg enqueue: {}", resp->getOrString("error"));
    return 1;
  }
  std::println("{}", resp->getOrString("id"));
  return 0;
}

}  // namespace

// `bus msg enqueue TOPIC BODY [--protocol TAG] [--deliver-when WHEN] [--ttl MS]`
auto subEnqueue(std::span<const char* const> args) -> int {
  if (args.size() < 2) {
    std::println(stderr,
                 "usage: bus msg enqueue TOPIC BODY "
                 "[--protocol TAG] [--deliver-when immediate|idle] [--ttl MS]");
    return 2;
  }
  const std::string topic{args[0]};
  const std::string body{args[1]};
  std::string protocol = "text";
  std::string deliver_when = "immediate";
  std::int64_t ttl_ms = 0;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--protocol") {
      if (++i >= args.size()) return 2;
      protocol = args[i];
    } else if (a == "--deliver-when") {
      if (++i >= args.size()) return 2;
      deliver_when = args[i];
    } else if (a == "--ttl") {
      if (++i >= args.size()) return 2;
      ttl_ms = std::atoll(args[i]);
    } else {
      std::println(stderr, "bus msg enqueue: unknown flag \"{}\"", a);
      return 2;
    }
  }
  return callEnqueue(topic, body, protocol, deliver_when, ttl_ms);
}

// `bus msg mail AGENT BODY [--ttl MS]` — enqueue to inbox-AGENT
// (auto-created agent-inbox).
//
// NOTE: --title is still accepted (so existing callers don't break) but
// is now INERT — it no longer writes the monitor TASK column. Chatty
// mail subjects were clobbering that column; set an agent's current
// task deliberately with `bus task set AGENT "<action>"` instead.
auto subMail(std::span<const char* const> args) -> int {
  std::int64_t ttl_ms = kDefaultMailTtlMs;
  std::vector<std::string_view> positional;
  positional.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--title") {
      if (++i >= args.size()) {
        std::println(stderr, "usage: bus msg mail AGENT BODY [--ttl MS]");
        return 2;
      }
      // Accepted for backward compat, intentionally ignored — the TASK
      // column is owned by `bus task set` now.
    } else if (a == "--ttl") {
      if (++i >= args.size()) return 2;
      ttl_ms = std::atoll(args[i]);
    } else if (a == "--") {
      while (++i < args.size()) positional.emplace_back(args[i]);
      break;
    } else if (a.starts_with("--")) {
      std::println(stderr, "bus msg mail: unknown flag \"{}\"", a);
      return 2;
    } else {
      positional.emplace_back(a);
    }
  }
  if (positional.size() != 2) {
    std::println(stderr, "usage: bus msg mail AGENT BODY [--ttl MS]");
    return 2;
  }
  const std::string agent{positional[0]};
  const std::string body{positional[1]};
  const std::string topic = std::string{"inbox-"} + agent;

  return callEnqueue(topic, body, "text", "immediate", ttl_ms);
}

// `bus task set AGENT "<action>"` — set the agent's monitor TASK column
// ($STATE/title/AGENT). The DELIBERATE task setter, split out from mail
// so chatty `bus msg mail` subjects can't clobber the column. An empty
// action clears it; the broker also clears it on the agent's /clear (a
// fresh session has no current task — see delivery.cpp scanEvents).
auto subTask(std::span<const char* const> args) -> int {
  if (args.size() < 2 || std::string_view{args[0]} != "set") {
    std::println(stderr, "usage: bus task set AGENT \"<action>\"");
    return 2;
  }
  const std::string agent{args[1]};
  // Join any trailing args so an unquoted multi-word action still works;
  // no args after AGENT clears the column.
  std::string action;
  for (std::size_t i = 2; i < args.size(); ++i) {
    if (!action.empty()) action += ' ';
    action += args[i];
  }

  const std::string dir = bus::stateRoot() + "/title";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = dir + "/" + agent;
  if (action.empty()) {
    std::filesystem::remove(path, ec);
    return 0;
  }
  std::ofstream out{path};
  if (!out) {
    std::println(stderr, "bus task set: cannot write {}", path);
    return 1;
  }
  out << action << '\n';
  return 0;
}

// `bus msg broadcast TAG BODY --to AGENTS` — fan out one body into each
// agent's inbox. Doesn't go through a pubsub topic; this is a
// sender-side helper that just enqueues N times. TAG becomes the
// `protocol` field on every fanned-out record so recipients can
// distinguish broadcasts from direct mail.
//
// Example:
//   bus msg broadcast deploy "starting" --to primary,elodin,bast
//
// Sends three records:
//   inbox-primary  protocol=deploy body="starting"
//   inbox-elodin   protocol=deploy body="starting"
//   inbox-bast     protocol=deploy body="starting"
auto subBroadcast(std::span<const char* const> args) -> int {
  if (args.size() < 2) {
    std::println(stderr,
                 "usage: bus msg broadcast TAG BODY --to AGENTS  "
                 "[--ttl MS]  (AGENTS comma-separated)");
    return 2;
  }
  const std::string tag{args[0]};
  const std::string body{args[1]};
  std::string to_csv;
  std::int64_t ttl_ms = kDefaultMailTtlMs;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--to") {
      if (++i >= args.size()) return 2;
      to_csv = args[i];
    } else if (a == "--ttl") {
      if (++i >= args.size()) return 2;
      ttl_ms = std::atoll(args[i]);
    } else {
      std::println(stderr, "bus msg broadcast: unknown flag \"{}\"", a);
      return 2;
    }
  }
  if (to_csv.empty()) {
    std::println(stderr, "bus msg broadcast: --to AGENTS is required");
    return 2;
  }

  // Split AGENTS on commas.
  std::vector<std::string> agents;
  std::string cur;
  for (char c : to_csv) {
    if (c == ',') {
      if (!cur.empty()) agents.push_back(std::move(cur));
      cur.clear();
    } else if (c != ' ' && c != '\t') {
      cur += c;
    }
  }
  if (!cur.empty()) agents.push_back(std::move(cur));
  if (agents.empty()) {
    std::println(stderr, "bus msg broadcast: --to AGENTS resolved to empty list");
    return 2;
  }

  int rc = 0;
  for (const auto& agent : agents) {
    const std::string topic = std::string{"inbox-"} + agent;
    if (callEnqueue(topic, body, tag, "immediate", ttl_ms) != 0) rc = 1;
  }
  return rc;
}

// `bus msg slash AGENT /command` — enqueue to commands-AGENT (auto-created
// tui-commands). Default delivery is `idle` to avoid mid-response races.
auto subSlash(std::span<const char* const> args) -> int {
  std::vector<std::string_view> positional;
  positional.reserve(args.size());
  std::int64_t ttl_ms = kDefaultSlashTtlMs;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--ttl") {
      if (++i >= args.size()) return 2;
      ttl_ms = std::atoll(args[i]);
    } else if (a.starts_with("--")) {
      std::println(stderr, "bus msg slash: unknown flag \"{}\"", a);
      return 2;
    } else {
      positional.emplace_back(a);
    }
  }
  if (positional.size() != 2) {
    std::println(stderr, "usage: bus msg slash AGENT /command [--ttl MS]");
    return 2;
  }
  const std::string body{positional[1]};
  if (body.empty() || body[0] != '/') {
    std::println(stderr, "bus msg slash: body must start with '/' (got \"{}\")",
                 body);
    return 2;
  }
  const std::string topic = std::string{"commands-"} + std::string{positional[0]};
  return callEnqueue(topic, body, "slash", "idle", ttl_ms);
}

}  // namespace bus
