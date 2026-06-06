// `bus msg fetch TOPIC` / `bus msg peek TOPIC` / `bus msg body MSG_ID` — consume /
// inspect topic records via broker RPC.

#include "../broker.h"
#include "../bus.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../sub.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

// ISO-8601 UTC millisecond timestamp, matching log-event.sh's `date -u
// +%FT%T.%3NZ` so bus-ack lines read like every other events.jsonl line.
auto isoNowMs() -> std::string {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  const auto ms =
      duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
  std::tm tm;
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
  return std::format("{}.{:03d}Z", buf, static_cast<int>(ms));
}

// Append one line to events.jsonl atomically (single O_APPEND write,
// well under the 4 KiB single-write atomicity bound), so it interleaves
// safely with the shell hooks' concurrent appends.
auto appendEventLine(const std::string& path, const std::string& line)
    -> void {
  const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (fd < 0) return;
  const std::string buf = line + "\n";
  [[maybe_unused]] const auto n = ::write(fd, buf.data(), buf.size());
  ::close(fd);
}

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

// `bus msg drain AGENT [EVENT]` — the OFF-TTY delivery hook engine
// (roadmap 2.1 / transport §5.1). Calls the broker's `drain` RPC to
// pull-consume the agent's own pending inbox records, then prints a
// Claude Code `additionalContext` JSON object so the calling
// UserPromptSubmit/SessionStart hook can inject the mail as a clean
// turn — no TTY write. Prints nothing (clean exit) when there's nothing
// to deliver or the presence gate deferred, so it's safe to run on
// every turn. EVENT defaults to UserPromptSubmit and sets hookEventName.
auto subDrain(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr, "usage: bus msg drain AGENT [EVENT]");
    return 2;
  }
  const std::string agent{args[0]};
  const std::string event = args.size() > 1 ? std::string{args[1]}
                                            : std::string{"UserPromptSubmit"};

  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("drain")});
  req.insert({"agent", json::Value::from(agent)});
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus msg drain: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus msg drain: {}", resp->getOrString("error"));
    return 1;
  }

  const auto* msgs = resp->get("messages");
  if (msgs == nullptr || !msgs->isArray() || msgs->asArray().empty()) {
    return 0;  // nothing to deliver (or deferred) — emit no context
  }

  std::string ctx;
  std::vector<std::string> acked;  // msg_ids actually injected this drain
  for (const auto& m : msgs->asArray()) {
    if (!m.isObject()) continue;
    ctx += "## bus mail from ";
    ctx += m.getOrString("sender", "unknown");
    ctx += '\n';
    ctx += m.getOrString("body");
    ctx += "\n\n";
    if (const auto id = m.getOrString("id"); !id.empty()) acked.push_back(id);
  }
  if (ctx.empty()) return 0;

  std::map<std::string, json::Value> hso;
  hso.insert({"hookEventName", json::Value::from(event)});
  hso.insert({"additionalContext", json::Value::from(ctx)});
  std::map<std::string, json::Value> out;
  out.insert({"hookSpecificOutput", json::Value::fromObject(std::move(hso))});
  std::println("{}", json::serialize(json::Value::fromObject(std::move(out))));

  // D3: now that the records are committed to the model as
  // additionalContext, emit one {event:bus-ack,payload:{msg_id}} per
  // delivered record to events.jsonl. The broker's scanEvents acks by id
  // — advancing the inbox cursor + stamping the lastid marker — so a
  // record is consumed only once the agent has actually been handed it
  // (no bus-ack ⇒ the un-advanced cursor re-delivers next turn). stdout
  // above stays clean (it IS the hook's additionalContext payload).
  if (!acked.empty()) {
    const char* sess = std::getenv("CLAUDE_BUS_AGENT_SESSION");
    const auto ts = isoNowMs();
    const auto log_path = cfg.state_dir + "/events.jsonl";
    for (const auto& id : acked) {
      std::map<std::string, json::Value> payload;
      payload.insert({"msg_id", json::Value::from(id)});
      std::map<std::string, json::Value> line;
      line.insert({"ts", json::Value::from(ts)});
      line.insert({"session",
                   json::Value::from(std::string{sess ? sess : ""})});
      line.insert({"agent", json::Value::from(agent)});
      line.insert({"event", json::Value::from(std::string{"bus-ack"})});
      line.insert({"payload", json::Value::fromObject(std::move(payload))});
      appendEventLine(
          log_path, json::serialize(json::Value::fromObject(std::move(line))));
    }
  }
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

auto subDrop(std::span<const char* const> args) -> int {
  if (args.empty()) {
    std::println(stderr, "usage: bus msg drop MSG_ID");
    return 2;
  }
  const std::string msg_id{args[0]};

  // Stamp the audit row with the caller's agent id when known, so
  // dropping via the bus from inside an agent leaves a useful trail.
  const char* caller_env = std::getenv("CLAUDE_BUS_AGENT_ID");
  const std::string caller =
      (caller_env != nullptr && caller_env[0] != '\0') ? caller_env : "cli";

  const auto cfg = resolveConfig();
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("drop")});
  req.insert({"msg_id", json::Value::from(msg_id)});
  req.insert({"caller", json::Value::from(caller)});
  auto resp = rpc::call(cfg.socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus msg drop: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus msg drop: {}", resp->getOrString("error"));
    return 1;
  }
  std::println("dropped {} from {} (cursor → {})", msg_id,
               resp->getOrString("topic"),
               resp->getOrString("cursor_after"));
  return 0;
}

}  // namespace bus
