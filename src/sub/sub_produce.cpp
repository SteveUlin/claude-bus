// `bus msg enqueue TOPIC body [...]` — and sugar verbs `bus msg mail`, `bus msg slash`.

#include "../broker.h"
#include "../bus.h"
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
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

auto senderFromEnv() -> std::string {
  if (const char* env = std::getenv("CLAUDE_BUS_AGENT_ID")) return env;
  return "unknown";
}

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

// `bus msg mail AGENT BODY [--title TITLE]` — enqueue to inbox-AGENT
// (auto-created agent-inbox). When --title is set, also write the
// title to $STATE/title/AGENT so `bus monitor` can render it as the
// recipient's current "what is this context window about?" tag.
// Empty --title clears the existing file. Mails without --title leave
// any existing title in place — titles persist across follow-up
// dispatches until the sender explicitly updates them.
auto subMail(std::span<const char* const> args) -> int {
  std::string title;
  bool title_set = false;
  std::vector<std::string_view> positional;
  positional.reserve(args.size());
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--title") {
      if (++i >= args.size()) {
        std::println(stderr,
                     "usage: bus msg mail AGENT BODY [--title TITLE]");
        return 2;
      }
      title = args[i];
      title_set = true;
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
    std::println(stderr, "usage: bus msg mail AGENT BODY [--title TITLE]");
    return 2;
  }
  const std::string agent{positional[0]};
  const std::string body{positional[1]};
  const std::string topic = std::string{"inbox-"} + agent;

  // Write/clear the title file before the enqueue call so the next
  // monitor tick picks it up promptly. Enqueue failures don't roll
  // back the title — that's the sender's last intent regardless.
  if (title_set) {
    const char* env = std::getenv("CLAUDE_BUS_STATE");
    const std::string state_dir = env ? env : "/tmp/claude-bus";
    const std::string dir = state_dir + "/title";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/" + agent;
    if (title.empty()) {
      std::filesystem::remove(path, ec);
    } else {
      std::ofstream out{path};
      if (out) out << title << '\n';
    }
  }

  return callEnqueue(topic, body, "text", "immediate", 0);
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
                 "(AGENTS comma-separated)");
    return 2;
  }
  const std::string tag{args[0]};
  const std::string body{args[1]};
  std::string to_csv;
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--to") {
      if (++i >= args.size()) return 2;
      to_csv = args[i];
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
    if (callEnqueue(topic, body, tag, "immediate", 0) != 0) rc = 1;
  }
  return rc;
}

// `bus msg slash AGENT /command` — enqueue to commands-AGENT (auto-created
// tui-commands). Default delivery is `idle` to avoid mid-response races.
auto subSlash(std::span<const char* const> args) -> int {
  if (args.size() != 2) {
    std::println(stderr, "usage: bus msg slash AGENT /command");
    return 2;
  }
  const std::string body{args[1]};
  if (body.empty() || body[0] != '/') {
    std::println(stderr, "bus msg slash: body must start with '/' (got \"{}\")",
                 body);
    return 2;
  }
  const std::string topic = std::string{"commands-"} + args[0];
  return callEnqueue(topic, body, "slash", "idle", 0);
}

}  // namespace bus
