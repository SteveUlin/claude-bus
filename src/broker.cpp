#include "broker.h"

#include "agent_status.h"
#include "delivery.h"
#include "json_min.h"
#include "rpc.h"
#include "topic_log.h"
#include "topic_registry.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <set>
#include <sstream>
#include <string>

namespace bus {

namespace {

auto envOr(const char* var, const char* def) -> std::string {
  const char* v = std::getenv(var);
  return v ? v : def;
}

// Read the contents of `path`, trimmed. Empty on miss.
auto readFileTrimmed(const std::string& path) -> std::string {
  std::ifstream in{path};
  if (!in) return {};
  std::string s;
  std::getline(in, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                        s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

// Append one structured line to broker.log. Format:
//   ISO8601 TAG key=val key=val ...
// Also writes to stderr when stderr is a TTY so race-loser invocations
// of `bus broker run` show feedback at the launching pane.
auto logEvent(const std::string& state_dir, const char* tag,
              const std::string& fields) -> void {
  using namespace std::chrono;
  const auto t = system_clock::to_time_t(system_clock::now());
  std::tm tm;
  ::gmtime_r(&t, &tm);
  char ts[24];
  std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
  const auto line = std::format("{} {:<5} {}\n", ts, tag, fields);

  const std::string log_path = state_dir + "/broker.log";
  const int fd =
      ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    auto _ig = ::write(fd, line.data(), line.size());
    (void)_ig;
    ::close(fd);
  }
  if (::isatty(STDERR_FILENO)) {
    std::print(stderr, "{}", line);
  }
}

// Read start time of a process (ms since epoch) from /proc/<pid>/stat
// combined with /proc/stat btime. Returns 0 when unavailable.
auto procStartedMs(long pid) -> std::int64_t {
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream in{path};
  if (!in) return 0;
  // /proc/<pid>/stat: pid (comm) state ppid ... starttime is field 22.
  // comm can contain spaces and parens — skip to the LAST ')' on the
  // first line before splitting the rest by whitespace.
  std::string line;
  std::getline(in, line);
  const auto rparen = line.rfind(')');
  if (rparen == std::string::npos) return 0;
  std::istringstream rest{line.substr(rparen + 1)};
  std::string field;
  // After "(comm)" the next field is `state` (#3). We want field 22
  // (starttime), so skip 19 more.
  for (int i = 0; i < 19; ++i) {
    if (!(rest >> field)) return 0;
  }
  long long starttime_ticks = 0;
  if (!(rest >> starttime_ticks)) return 0;

  const long hz = ::sysconf(_SC_CLK_TCK);
  if (hz <= 0) return 0;

  std::ifstream stat{"/proc/stat"};
  if (!stat) return 0;
  long long btime_secs = 0;
  std::string key;
  while (stat >> key) {
    if (key == "btime") {
      stat >> btime_secs;
      break;
    }
    std::getline(stat, key);  // skip rest of line
  }
  if (btime_secs <= 0) return 0;
  const long long started_secs = btime_secs + starttime_ticks / hz;
  return static_cast<std::int64_t>(started_secs) * 1000;
}

// Broker epoch. A small unsigned counter bumped on every successful
// boot, persisted to $STATE/broker.epoch. Stamped onto each record at
// enqueue and checked at dispatch — a record whose epoch doesn't
// match the current broker's epoch is quarantined (escalated +
// cursor-advanced) rather than delivered. Survives the wipe-on-boot
// of volatile state below.
auto readEpoch(const std::string& path) -> std::uint64_t {
  std::ifstream in{path, std::ios::binary};
  if (!in) return 0;
  std::uint64_t v = 0;
  in.read(reinterpret_cast<char*>(&v), sizeof(v));
  return in ? v : 0;
}

auto writeEpoch(const std::string& path, std::uint64_t v) -> bool {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
    if (!out) return false;
  }
  return ::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace

auto resolveConfig() -> BrokerConfig {
  BrokerConfig cfg;
  cfg.state_dir = envOr("CLAUDE_BUS_STATE", "/tmp/claude-bus");
  cfg.socket_path = cfg.state_dir + "/broker.sock";
  cfg.pid_path = cfg.state_dir + "/broker.pid";
  return cfg;
}

auto runBroker(const BrokerConfig& cfg) -> int {
  std::error_code ec;
  std::filesystem::create_directories(cfg.state_dir, ec);
  if (ec) {
    std::println(stderr, "broker: cannot create {}: {}", cfg.state_dir,
                 ec.message());
    return 1;
  }

  // Singleton guard via flock on the pid file.
  //
  // History: we used to gate on O_EXCL + kill(pid,0). That failed when
  // the previous broker's binary got rebuilt out from under it: the
  // process kept running with a deleted exe, the pid file was somehow
  // reset, and a new broker happily started alongside. Two brokers in
  // one bus.
  //
  // flock survives unlink, dies with the process, and serializes
  // concurrent broker starts atomically. We hold the fd for the
  // broker's lifetime; closing on shutdown releases the lock.
  const int pidfd = ::open(cfg.pid_path.c_str(),
                           O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (pidfd < 0) {
    std::println(stderr, "broker: cannot open pid file {}: {}",
                 cfg.pid_path, std::strerror(errno));
    return 1;
  }
  if (::flock(pidfd, LOCK_EX | LOCK_NB) < 0) {
    const auto holder_str = readFileTrimmed(cfg.pid_path);
    const long holder =
        holder_str.empty() ? 0 : std::atol(holder_str.c_str());
    std::int64_t alive_ms = 0;
    if (holder > 0) {
      const auto started = procStartedMs(holder);
      if (started > 0) alive_ms = nowMs() - started;
    }
    logEvent(cfg.state_dir, "DEFER",
             std::format("pid={} holder_pid={} holder_alive_ms={}",
                         ::getpid(),
                         holder_str.empty() ? "?" : holder_str,
                         alive_ms));
    ::close(pidfd);
    return 1;
  }
  // We own the lock. Stomp any stale pid value with our own.
  if (::ftruncate(pidfd, 0) < 0) { /* not fatal */ }
  ::lseek(pidfd, 0, SEEK_SET);
  {
    const auto pid_str = std::to_string(::getpid()) + "\n";
    if (::write(pidfd, pid_str.data(), pid_str.size()) < 0) {
      // not fatal; the lock is the meaningful artifact
    }
  }
  // Keep pidfd open for the rest of runBroker — releasing the lock
  // requires closing it.

  // Surgical wipe: only the per-boot session state. Pre-feature this
  // block removed everything except broker.pid + events.jsonl, which
  // re-delivered no records ever — at the cost of throwing away
  // audit-worthy topic logs on every restart. The epoch quarantine
  // below replaces that hammer with a scalpel: durable record store
  // persists, pre-boot records get audited + cursor-skipped on
  // dispatch.
  //
  // Wipe:
  //   - in-flight/   per-dispatch tracker; stale entries point at
  //                  records that need a fresh dispatch decision
  //   - presence/    [bus-attach] sentinels; the agents that wrote
  //                  them are gone or about to re-attach
  //   - tui-locks/   per-pane flocks; pane lifecycles are session
  //                  scoped, the inodes are stale anyway
  //   - broker.sock  rpc::Server::bind unlinks it too, but be explicit
  //
  // Preserve: topics/, cursors/, topics.json, agents/, payloads/,
  // broker.log, broker.epoch (read below), events.jsonl, broker.pid.
  for (const auto& d : {"in-flight", "presence", "tui-locks"}) {
    std::filesystem::remove_all(cfg.state_dir + "/" + d, ec);
  }
  std::filesystem::remove(cfg.state_dir + "/broker.sock", ec);

  // Bump the broker epoch. The previous run's epoch (or 0 on a
  // fresh state dir) is the floor; we increment + persist + carry
  // the new value into the Loop so it can quarantine records still
  // sitting on disk from before this boot.
  const std::string epoch_path = cfg.state_dir + "/broker.epoch";
  const auto current_epoch = readEpoch(epoch_path) + 1;
  if (!writeEpoch(epoch_path, current_epoch)) {
    logEvent(cfg.state_dir, "WARN",
             std::format("epoch write failed (continuing): {}", epoch_path));
  }

  const auto started_ms = nowMs();
  logEvent(cfg.state_dir, "START",
           std::format("pid={} socket={} epoch={}", ::getpid(),
                       cfg.socket_path, current_epoch));

  // Topic registry: lives in process memory, persists to topics.json.
  TopicRegistry registry{cfg.state_dir + "/topics.json"};
  if (auto r = registry.load(); !r) {
    logEvent(cfg.state_dir, "ERROR",
             std::format("registry load failed: {}", r.error().message));
    ::unlink(cfg.pid_path.c_str());
    ::close(pidfd);
    return 1;
  }
  logEvent(cfg.state_dir, "LOAD",
           std::format("registry topics={}", registry.list().size()));

  rpc::Server server{cfg.socket_path};

  server.on("ping", [](const json::Value&) {
    return json::okResponse();
  });
  server.on("stop", [](const json::Value&) {
    rpc::Server::requestStop();
    return json::okResponse();
  });
  server.on("info", [&](const json::Value&) {
    std::map<std::string, json::Value> m;
    m.insert(
        {"pid", json::Value::from(static_cast<std::int64_t>(::getpid()))});
    m.insert({"started_ms", json::Value::from(started_ms)});
    m.insert({"uptime_ms", json::Value::from(nowMs() - started_ms)});
    m.insert({"socket", json::Value::from(cfg.socket_path)});
    m.insert({"state_dir", json::Value::from(cfg.state_dir)});
    m.insert({"topic_count",
              json::Value::from(
                  static_cast<std::int64_t>(registry.list().size()))});
    return json::okResponse(std::move(m));
  });

  server.on("topic_create", [&registry](const json::Value& req) {
    TopicConfig tc;
    tc.name = req.getOrString("name");
    tc.kind = req.getOrString("kind");
    tc.max_record_bytes = req.getOrInt("max_record_bytes", 4096);
    tc.retention_ms = req.getOrInt("retention_ms", 0);
    if (const auto* kc = req.get("kind_config"); kc != nullptr) {
      tc.kind_config = *kc;
    }
    if (auto r = registry.create(std::move(tc)); !r) {
      return json::errorResponse(r.error().message);
    }
    return json::okResponse();
  });

  server.on("topic_list", [&registry](const json::Value&) {
    std::vector<json::Value> arr;
    for (const auto& cfg : registry.list()) arr.push_back(cfg.toJson());
    std::map<std::string, json::Value> m;
    m.insert({"topics", json::Value::fromArray(std::move(arr))});
    return json::okResponse(std::move(m));
  });

  server.on("topic_show", [&registry](const json::Value& req) {
    const auto name = req.getOrString("name");
    if (name.empty()) return json::errorResponse("missing name");
    const auto* cfg = registry.get(name);
    if (cfg == nullptr) {
      return json::errorResponse(std::string{"no such topic: "} + name);
    }
    std::map<std::string, json::Value> m;
    m.insert({"topic", cfg->toJson()});
    return json::okResponse(std::move(m));
  });

  // -- enqueue / peek / fetch -------------------------------------
  //
  // Topic logs live on disk under $STATE/topics/<name>.log; we open a
  // TopicLog lazily on first access and keep a cache so subsequent
  // calls don't re-stat the registry path.
  const std::string topics_dir = cfg.state_dir + "/topics";
  std::map<std::string, topic::TopicLog> logs;

  auto getOrOpenLog = [&](std::string_view name) -> topic::TopicLog& {
    auto it = logs.find(std::string{name});
    if (it != logs.end()) return it->second;
    const std::string path =
        topics_dir + "/" + std::string{name} + ".log";
    auto [ins, _] =
        logs.emplace(std::string{name}, topic::TopicLog{path});
    return ins->second;
  };

  server.on("enqueue", [&](const json::Value& req) {
    const auto name = req.getOrString("topic");
    if (name.empty()) return json::errorResponse("missing topic");
    if (auto r = registry.getOrAutoCreate(name); !r) {
      return json::errorResponse(r.error().message);
    }
    const auto sender = req.getOrString("sender", "unknown");
    const auto body = req.getOrString("body");

    topic::SendOpts opts;
    opts.ttl_ms = static_cast<std::uint32_t>(req.getOrInt("ttl_ms", 0));
    const auto dw = req.getOrString("deliver_when", "immediate");
    opts.deliver_when = (dw == "idle") ? 1 : 0;
    opts.protocol = req.getOrString("protocol", "text");
    // Stamp the broker epoch so dispatch can quarantine records that
    // outlive a broker restart. Same epoch on the pubsub cascade
    // below so subscribers all see consistent provenance.
    delivery::stampEpoch(opts, current_epoch);

    auto& log = getOrOpenLog(name);
    auto r = log.append(sender, body, opts);
    if (!r) return json::errorResponse(r.error().message);

    const auto* tcfg = registry.get(name);

    // pubsub cascade: when the enqueue lands on a pubsub topic, also
    // copy the record into each declared subscriber's inbox-<name>.
    // The pubsub topic itself keeps the canonical record (audit /
    // replay); each subscriber gets a delivery copy via its inbox-
    // <name> agent-inbox topic (which the broker's delivery loop
    // pushes to the recipient pane).
    if (tcfg != nullptr && tcfg->kind == std::string{kKindPubsub}) {
      const auto* subs_v = tcfg->kind_config.get("subscribers");
      if (subs_v != nullptr && subs_v->isArray()) {
        for (const auto& sub : subs_v->asArray()) {
          if (!sub.isString()) continue;
          const auto inbox = std::string{"inbox-"} + sub.asString();
          if (auto cr = registry.getOrAutoCreate(inbox); !cr) continue;
          auto& sublog = getOrOpenLog(inbox);
          auto _ig = sublog.append(sender, body, opts);
        }
      }
    }

    // blackboard: advance the default cursor to point AT the new
    // record so peek/fetch return only the latest. Older records
    // stay on disk for audit but become unreachable through normal
    // reads.
    if (tcfg != nullptr && tcfg->kind == std::string{kKindBlackboard}) {
      auto all = log.dump();
      if (all && !all->empty()) {
        const auto& latest = all->back();
        const auto cursor_p = topic::cursorPath(cfg.state_dir, name, "");
        topic::writeCursor(cursor_p, latest.offset);
      }
    }

    std::map<std::string, json::Value> m;
    m.insert({"id", json::Value::from(*r)});
    return json::okResponse(std::move(m));
  });

  auto messageToJson = [](const topic::Message& m) {
    std::map<std::string, json::Value> o;
    o.insert({"id", json::Value::from(m.id)});
    o.insert({"sent_ms", json::Value::from(m.sent_ms)});
    o.insert({"sender", json::Value::from(m.sender)});
    o.insert({"ttl_ms",
              json::Value::from(static_cast<std::int64_t>(m.ttl_ms))});
    o.insert({"deliver_when",
              json::Value::from(static_cast<std::int64_t>(m.deliver_when))});
    o.insert({"protocol", json::Value::from(m.protocol)});
    o.insert({"body", json::Value::from(m.body)});
    o.insert({"offset", json::Value::from(m.offset)});
    o.insert({"next_offset", json::Value::from(m.next_offset)});
    return json::Value::fromObject(std::move(o));
  };

  server.on("peek", [&, messageToJson](const json::Value& req) {
    const auto name = req.getOrString("topic");
    if (name.empty()) return json::errorResponse("missing topic");
    if (!registry.contains(name)) {
      return json::errorResponse(std::string{"no such topic: "} + name);
    }
    const auto consumer = req.getOrString("consumer");
    const auto limit = req.getOrInt("limit", 0);

    const auto cursor_p = topic::cursorPath(cfg.state_dir, name, consumer);
    const auto cursor = topic::readCursor(cursor_p);
    const auto start =
        cursor > 0 ? cursor : static_cast<std::int64_t>(topic::kFileHeaderBytes);

    auto& log = getOrOpenLog(name);
    auto r = log.peek(start,
                      limit > 0 ? static_cast<std::size_t>(limit) : SIZE_MAX);
    if (!r) return json::errorResponse(r.error().message);
    std::vector<json::Value> arr;
    const auto now = nowMs();
    for (const auto& m : *r) {
      if (m.ttl_ms != 0 && m.sent_ms + static_cast<std::int64_t>(m.ttl_ms) < now) {
        continue;
      }
      arr.push_back(messageToJson(m));
    }
    std::map<std::string, json::Value> resp;
    resp.insert({"messages", json::Value::fromArray(std::move(arr))});
    return json::okResponse(std::move(resp));
  });

  // -- state --------------------------------------------------------
  //
  // Broker's view of each agent's lifecycle. Derived from events.jsonl
  // (which hooks write to) — reuses bus::readAgents + bus::computeState
  // so monitor / agent-bar / broker all agree on the state names.
  //
  // For phase 4c.1 we recompute on each `state` RPC call. Phase 4c.2
  // adds an inotify-driven cache so the delivery loop can react to
  // state transitions without polling.
  server.on("state", [&](const json::Value& req) {
    const std::string log_path = "/tmp/claude-bus/events.jsonl";
    const auto wanted = req.getOrString("agent");
    std::set<std::string> filter;
    if (!wanted.empty()) filter.insert(wanted);
    auto agents = readAgents(log_path, filter);
    if (!wanted.empty() && !agents.contains(wanted)) {
      agents[wanted] = {};
    }
    const auto now = nowMs();
    std::map<std::string, json::Value> out;
    for (const auto& [name, info] : agents) {
      const auto pane = paneState(name);

      // Unread count from the agent's inbox topic. Was previously zeroed
      // on the broker side; viewers now consume it from here instead of
      // peeking the topic log themselves, so the count must be honest.
      std::size_t unread = 0;
      {
        const auto topic = std::string{"inbox-"} + name;
        const auto inbox_path =
            cfg.state_dir + "/topics/" + topic + ".log";
        const auto cursor_p = topic::cursorPath(cfg.state_dir, topic, "");
        const auto cursor = topic::readCursor(cursor_p);
        const auto start =
            cursor > 0 ? cursor
                       : static_cast<std::int64_t>(topic::kFileHeaderBytes);
        topic::TopicLog inbox_log{inbox_path};
        auto r = inbox_log.peek(start);
        if (r) unread = r->size();
      }

      const auto ax = computeAxes(info, unread, now, pane.ok, &pane);
      const auto st = computeState(info, unread, now, pane.ok, &pane);

      std::map<std::string, json::Value> entry;
      entry.insert({"state", json::Value::from(std::string{stateName(st)})});

      std::map<std::string, json::Value> axes;
      axes.insert({"process",
                   json::Value::from(std::string{axisName(ax.process)})});
      axes.insert({"turn",
                   json::Value::from(std::string{axisName(ax.turn)})});
      axes.insert({"mail",
                   json::Value::from(std::string{axisName(ax.mail)})});
      axes.insert({"tui",
                   json::Value::from(std::string{axisName(ax.tui)})});
      entry.insert({"axes", json::Value::fromObject(std::move(axes))});

      entry.insert({"last_event", json::Value::from(info.last.event)});
      entry.insert({"last_tool", json::Value::from(info.last.tool)});
      entry.insert(
          {"last_source", json::Value::from(info.last.source)});
      entry.insert(
          {"last_notification_type",
           json::Value::from(info.last.notification_type)});
      entry.insert({"last_ts_ms", json::Value::from(info.last.ts_ms)});
      entry.insert({"age_ms",
                    json::Value::from(info.last.ts_ms > 0
                                          ? now - info.last.ts_ms
                                          : -1)});
      entry.insert({"pane_exists", json::Value::from(pane.ok)});
      entry.insert({"unread",
                    json::Value::from(static_cast<std::int64_t>(unread))});

      // TUI details — viewers display these directly; not derived from
      // events. Empty strings when the pane scan failed.
      entry.insert({"mode", json::Value::from(pane.mode)});
      entry.insert({"buffer", json::Value::from(pane.buffer)});
      entry.insert(
          {"bypass_perms", json::Value::from(pane.bypass_perms)});

      entry.insert(
          {"attached", json::Value::from(hasPresenceFile(name))});
      out.insert({name, json::Value::fromObject(std::move(entry))});
    }
    std::map<std::string, json::Value> resp;
    resp.insert({"state", json::Value::fromObject(std::move(out))});
    return json::okResponse(std::move(resp));
  });

  server.on("fetch", [&, messageToJson](const json::Value& req) {
    const auto name = req.getOrString("topic");
    if (name.empty()) return json::errorResponse("missing topic");
    if (!registry.contains(name)) {
      return json::errorResponse(std::string{"no such topic: "} + name);
    }
    const auto consumer = req.getOrString("consumer");
    const auto* tcfg = registry.get(name);
    const bool is_blackboard =
        tcfg != nullptr && tcfg->kind == std::string{kKindBlackboard};

    const auto cursor_p = topic::cursorPath(cfg.state_dir, name, consumer);
    const auto cursor = topic::readCursor(cursor_p);
    const auto start =
        cursor > 0 ? cursor : static_cast<std::int64_t>(topic::kFileHeaderBytes);

    auto& log = getOrOpenLog(name);
    auto r = log.peek(start, 1);
    if (!r) return json::errorResponse(r.error().message);
    if (r->empty()) {
      std::map<std::string, json::Value> resp;
      resp.insert({"message", json::Value::null_()});
      return json::okResponse(std::move(resp));
    }
    const auto& m = r->front();
    // blackboard: fetch is a non-destructive read — repeated calls
    // return the same value. For all other kinds, advance the cursor.
    if (!is_blackboard) {
      if (!topic::writeCursor(cursor_p, m.next_offset)) {
        return json::errorResponse("cursor write failed");
      }
    }
    std::map<std::string, json::Value> resp;
    resp.insert({"message", messageToJson(m)});
    return json::okResponse(std::move(resp));
  });

  // Resolve a msg_id to its body across all topics. Pure read — no
  // cursor advancement, no ack, no in-flight changes. Spilled bodies
  // are loaded transparently by TopicLog::peek, so the returned body
  // is the full content regardless of inline-vs-pointer storage.
  server.on("body", [&, messageToJson](const json::Value& req) {
    const auto msg_id = req.getOrString("msg_id");
    if (msg_id.empty()) return json::errorResponse("missing msg_id");
    for (const auto& tcfg : registry.list()) {
      auto& log = getOrOpenLog(tcfg.name);
      auto all = log.dump();
      if (!all) continue;
      for (const auto& m : *all) {
        if (m.id != msg_id) continue;
        std::map<std::string, json::Value> resp;
        resp.insert({"topic", json::Value::from(tcfg.name)});
        resp.insert({"message", messageToJson(m)});
        return json::okResponse(std::move(resp));
      }
    }
    return json::errorResponse(std::string{"no such msg_id: "} + msg_id);
  });

  // Delivery loop runs on each pselect tick (every 250ms), serially
  // with RPC handlers. Same thread → no locking around registry /
  // topic logs / in-flight tracker. Construct before binding the
  // socket so handlers below can capture `dl`.
  delivery::Loop dl{cfg, registry, current_epoch};
  dl.load();

  // Drop a record by msg_id without delivering — advance the topic
  // cursor past it, forget any in-flight entry, and append an audit
  // record. Used when a queued task is obsolete (e.g., the broker
  // wedged before ack, or the producer changed its mind). Different
  // from `fetch` (which is destructive on most kinds but still treated
  // as a "real" consume); drop is the explicit "throw this away"
  // verb. Returns ok with {topic, cursor_after, was_inflight}.
  server.on("drop", [&, &dl_ref = dl](const json::Value& req) {
    const auto msg_id = req.getOrString("msg_id");
    if (msg_id.empty()) return json::errorResponse("missing msg_id");

    // Locate the record across all known topics. The msg_id alone
    // doesn't tell us which topic owns it, and a single message can
    // sit on multiple topics (pubsub cascade), so prefer the topic
    // recorded in the in-flight entry when one exists.
    std::string found_topic;
    std::int64_t next_offset = -1;
    if (const auto inflight_snap = dl_ref.forgetInflight(msg_id);
        inflight_snap.has_value()) {
      found_topic = inflight_snap->topic;
      next_offset = inflight_snap->cursor_after;
    } else {
      for (const auto& tcfg : registry.list()) {
        auto& log = getOrOpenLog(tcfg.name);
        auto all = log.dump();
        if (!all) continue;
        for (const auto& m : *all) {
          if (m.id != msg_id) continue;
          found_topic = tcfg.name;
          next_offset = m.next_offset;
          break;
        }
        if (!found_topic.empty()) break;
      }
    }

    if (found_topic.empty() || next_offset < 0) {
      return json::errorResponse(std::string{"no such msg_id: "} +
                                 msg_id);
    }

    // Advance the topic's default cursor past the dropped record so
    // it never gets dispatched again. Only move forward — never
    // backward — so a drop on an already-consumed id is a no-op.
    const auto cursor_p =
        topic::cursorPath(cfg.state_dir, found_topic, "");
    const auto cur = topic::readCursor(cursor_p);
    if (next_offset > cur) {
      if (!topic::writeCursor(cursor_p, next_offset)) {
        return json::errorResponse("cursor write failed");
      }
    }

    // Audit the drop. Auto-create the audit topic on first use so a
    // fresh-bus drop still records a trail.
    {
      TopicConfig audit;
      audit.name = "audit";
      audit.kind = std::string{kKindAppendLog};
      if (!registry.contains("audit")) {
        auto _ig = registry.create(audit);
      }
      auto& audit_log = getOrOpenLog("audit");
      topic::SendOpts opts;
      opts.protocol = "drop";
      delivery::stampEpoch(opts, current_epoch);
      const auto entry = std::format(
          "drop msg_id={} topic={} next_offset={} caller={}",
          msg_id, found_topic, next_offset,
          req.getOrString("caller", "unknown"));
      auto _ig2 = audit_log.append("broker", entry, opts);
    }

    logEvent(cfg.state_dir, "DROP",
             std::format("msg_id={} topic={} cursor_after={}",
                         msg_id, found_topic, next_offset));

    std::map<std::string, json::Value> resp;
    resp.insert({"topic", json::Value::from(found_topic)});
    resp.insert({"cursor_after", json::Value::from(next_offset)});
    return json::okResponse(std::move(resp));
  });

  if (!server.bind()) {
    ::unlink(cfg.pid_path.c_str());
    ::close(pidfd);
    return 1;
  }

  // Expose in-flight via the `inflight` RPC for debugging.
  server.on("inflight", [&dl](const json::Value&) {
    std::vector<json::Value> arr;
    for (const auto& [id, f] : dl.inFlight()) {
      std::map<std::string, json::Value> o;
      o.insert({"msg_id", json::Value::from(f.msg_id)});
      o.insert({"topic", json::Value::from(f.topic)});
      o.insert({"agent", json::Value::from(f.agent)});
      o.insert(
          {"dispatched_at_ms", json::Value::from(f.dispatched_at_ms)});
      o.insert({"cursor_after", json::Value::from(f.cursor_after)});
      arr.push_back(json::Value::fromObject(std::move(o)));
    }
    std::map<std::string, json::Value> resp;
    resp.insert({"in_flight", json::Value::fromArray(std::move(arr))});
    return json::okResponse(std::move(resp));
  });

  const int rc = server.run(std::chrono::milliseconds{250},
                            [&dl]() { dl.tick(); });

  ::unlink(cfg.pid_path.c_str());
  ::close(pidfd);  // releases the singleton flock
  logEvent(cfg.state_dir, "STOP",
           std::format("pid={} uptime_ms={}", ::getpid(),
                       nowMs() - started_ms));
  return rc;
}

}  // namespace bus
