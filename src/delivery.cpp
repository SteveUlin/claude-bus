#include "delivery.h"

#include "agent_status.h"
#include "dispatch.h"
#include "json_min.h"
#include "pane.h"
#include "topic_log.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

namespace bus::delivery {

auto ackTimeoutMs() -> std::int64_t {
  if (const char* env = std::getenv("CLAUDE_BUS_ACK_TIMEOUT_MS")) {
    const auto v = std::atoll(env);
    if (v > 0) return v;
  }
  return 60 * 1000;  // 60s default
}

namespace {

auto inflightDir(const BrokerConfig& cfg) -> std::string {
  return cfg.state_dir + "/in-flight";
}

auto payloadsDir(const BrokerConfig& cfg) -> std::string {
  return cfg.state_dir + "/payloads";
}

auto inflightPath(const BrokerConfig& cfg, std::string_view msg_id)
    -> std::string {
  return inflightDir(cfg) + "/" + std::string{msg_id} + ".json";
}

auto fileSize(const std::string& path) -> std::int64_t {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<std::int64_t>(st.st_size);
}

// Write a small body to a pane, holding the per-pane flock. Returns
// true on success (mirrors what `bus msg send NAME TEXT` does).
auto deliverInline(const BrokerConfig& cfg, std::string_view agent,
                   std::string_view payload) -> bool {
  const auto pane = paneId(agent);
  if (pane.empty()) return false;

  const std::string lock_dir = cfg.state_dir + "/tui-locks";
  std::error_code ec;
  fs::create_directories(lock_dir, ec);
  const std::string lock_path =
      lock_dir + "/" + std::string{agent} + ".lock";

  const int fd =
      ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  while (::flock(fd, LOCK_EX) < 0) {
    if (errno != EINTR) {
      ::close(fd);
      return false;
    }
  }
  // sendToPaneSafe reads pane state, preserves the user's draft, and
  // refuses delivery when the pane is scrolled away from the prompt or
  // in a LOCKED modal. The flock guarantees no other writer mid-read.
  const bool ok = sendToPaneSafe(agent, payload);
  ::flock(fd, LOCK_UN);
  ::close(fd);
  return ok;
}

// Build the inline delivery payload for the recipient pane.
auto formatInlineBody(const topic::Message& m) -> std::string {
  std::string out;
  out += "## bus msg mail from ";
  out += m.sender;
  out += " [";
  out += m.protocol;
  out += "]";
  if (!m.body.empty()) {
    out += '\n';
    out += m.body;
  }
  return out;
}

// Build the pointer payload for large bodies. The recipient pulls the
// body through the bus API rather than slurping it into context. Use
// `bus msg body MSG_ID` (side-effect-free) instead of `bus msg fetch` so the
// read doesn't self-ack and bypass the broker's retry/escalation
// safety net.
auto formatPointerBody(const topic::Message& m) -> std::string {
  return std::format(
      "## bus msg mail from {} [{}] — large; read with: bus msg body {}",
      m.sender, m.protocol, m.id);
}

}  // namespace

Loop::Loop(const BrokerConfig& cfg, TopicRegistry& registry,
           std::uint64_t current_epoch)
    : cfg_{cfg}, registry_{registry}, current_epoch_{current_epoch} {}

auto Loop::blockingOpPath(std::string_view agent) const -> std::string {
  return cfg_.state_dir + "/blocking-op/" + std::string{agent};
}

auto Loop::setBlockingOp(std::string_view agent, std::string_view msg_id)
    -> void {
  std::error_code ec;
  fs::create_directories(cfg_.state_dir + "/blocking-op", ec);
  std::ofstream out{blockingOpPath(agent)};
  out << msg_id;
  blocking_ops_[std::string{agent}] = std::string{msg_id};
}

auto Loop::clearBlockingOp(std::string_view agent) -> void {
  std::error_code ec;
  fs::remove(blockingOpPath(agent), ec);
  blocking_ops_.erase(std::string{agent});
}

auto Loop::loadBlockingOps() -> void {
  blocking_ops_.clear();
  const auto dir = cfg_.state_dir + "/blocking-op";
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in{entry.path()};
    std::string id;
    std::getline(in, id);
    blocking_ops_.insert_or_assign(entry.path().filename().string(),
                                   std::move(id));
  }
}

auto Loop::load() -> void {
  // Read every $STATE/in-flight/*.json into memory. Survives restart.
  in_flight_.clear();
  const auto dir = inflightDir(cfg_);
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    std::ifstream in{entry.path()};
    if (!in) continue;
    std::ostringstream buf;
    buf << in.rdbuf();
    auto v = json::parse(buf.str());
    if (!v || !v->isObject()) continue;
    InFlight f;
    f.msg_id = v->getOrString("msg_id");
    f.topic = v->getOrString("topic");
    f.agent = v->getOrString("agent");
    f.dispatched_at_ms = v->getOrInt("dispatched_at_ms");
    f.cursor_after = v->getOrInt("cursor_after");
    f.attempts = static_cast<std::int32_t>(v->getOrInt("attempts", 1));
    f.next_retry_at = v->getOrInt("next_retry_at", 0);
    if (!f.msg_id.empty()) {
      in_flight_.insert_or_assign(f.msg_id, f);
    }
  }

  loadBlockingOps();
  // events_offset_ stays at -1 so the first tick seeks to EOF.
}

auto Loop::writeInflight(const InFlight& f) -> void {
  std::error_code ec;
  fs::create_directories(inflightDir(cfg_), ec);
  const auto path = inflightPath(cfg_, f.msg_id);
  std::map<std::string, json::Value> obj;
  obj.insert({"msg_id", json::Value::from(f.msg_id)});
  obj.insert({"topic", json::Value::from(f.topic)});
  obj.insert({"agent", json::Value::from(f.agent)});
  obj.insert({"dispatched_at_ms", json::Value::from(f.dispatched_at_ms)});
  obj.insert({"cursor_after", json::Value::from(f.cursor_after)});
  obj.insert({"attempts", json::Value::from(static_cast<std::int64_t>(
                              f.attempts))});
  obj.insert({"next_retry_at", json::Value::from(f.next_retry_at)});
  const auto wire = json::serialize(json::Value::fromObject(std::move(obj)));
  const std::string tmp = path + ".tmp";
  std::ofstream out{tmp};
  out << wire;
  out.close();
  if (out) ::rename(tmp.c_str(), path.c_str());
}

auto Loop::removeInflight(const std::string& msg_id) -> void {
  std::error_code ec;
  fs::remove(inflightPath(cfg_, msg_id), ec);
}

auto Loop::forgetInflight(const std::string& msg_id)
    -> std::optional<InFlight> {
  auto it = in_flight_.find(msg_id);
  if (it == in_flight_.end()) return std::nullopt;
  const auto snap = it->second;
  // Also clear blocking-op tracking when the dropped record is a
  // /clear or /compact dispatch; otherwise the agent stays trapped
  // behind the gate forever.
  if (blocking_ops_.contains(snap.agent) &&
      blocking_ops_.at(snap.agent) == msg_id) {
    clearBlockingOp(snap.agent);
  }
  removeInflight(msg_id);
  in_flight_.erase(it);
  return snap;
}

auto Loop::scanEvents() -> void {
  const std::string log = cfg_.state_dir + "/events.jsonl";
  const auto size = fileSize(log);

  if (events_offset_ < 0) {
    events_offset_ = size;  // first tick: seek to EOF, ignore history
    return;
  }
  if (size <= events_offset_) return;

  std::ifstream in{log};
  if (!in) return;
  in.seekg(events_offset_);

  std::string line;
  auto valid_pos = in.tellg();
  while (std::getline(in, line)) {
    if (in.eof()) {
      break; // partial line, wait for the rest
    }
    valid_pos = in.tellg();

    if (line.empty()) continue;
    auto v = json::parse(line);
    if (!v || !v->isObject()) continue;

    const auto event = v->getOrString("event");
    const auto agent = v->getOrString("agent");
    if (agent.empty()) continue;

    // Blocking-op ACK: either Stop (normal slash completion) or
    // SessionEnd reason=clear|compact (the slash killed the session
    // outright, so Stop never fires — claude restarts the session
    // instead). Without this second path, /clear and /compact stay
    // in-flight until retry exhaustion and the defer-gate in
    // dispatchAgentInbox keeps mail trapped behind them.
    const bool is_blocking_op_ack =
        event == "Stop" ||
        (event == "SessionEnd" &&
         (v->getOrString("reason") == "clear" ||
          v->getOrString("reason") == "compact"));
    if (is_blocking_op_ack && blocking_ops_.contains(agent)) {
      // The blocking-op msg_id is also tracked as in-flight (the
      // dispatch wrote it there). Clear both atomically.
      const auto& msg_id = blocking_ops_.at(agent);
      if (auto it = in_flight_.find(msg_id); it != in_flight_.end()) {
        const auto cursor_p =
            topic::cursorPath(cfg_.state_dir, it->second.topic, "");
        topic::writeCursor(cursor_p, it->second.cursor_after);
        removeInflight(msg_id);
        in_flight_.erase(it);
      }
      clearBlockingOp(agent);
      continue;
    }

    // Agent-death cleanup: SessionEnd with any reason OTHER than
    // clear/compact (those are restart-style, handled above). The
    // claude session has exited — could be a clean /exit, could be
    // a crash detected by the harness. Either way the in-flight
    // records targeting this agent are now bound to a TUI that
    // no longer exists; their dispatched-keystrokes vanished into
    // the dead pane and no ack will ever fire.
    //
    // Release the in-flight entries WITHOUT advancing the cursor.
    // The topic-log records stay at the head; if a new session
    // for the same agent starts (zellij respawn, agent-launch
    // re-resume), the next dispatch tick re-finds and re-delivers
    // them to the fresh TUI. If the agent stays dead, phase-2
    // hard-death detection (deferred) will eventually escalate.
    //
    // Also drop the presence sentinel — the [bus-attach] file
    // outlives its writer otherwise and silences delivery to the
    // (next) session. tui-locks are deliberately left alone:
    // they're per-call flock semantics, not session state.
    if (event == "SessionEnd") {
      std::vector<std::string> to_release;
      for (const auto& [id, f] : in_flight_) {
        if (f.agent == agent) to_release.push_back(id);
      }
      for (const auto& id : to_release) {
        removeInflight(id);
        in_flight_.erase(id);
      }
      // Mirror the blocking-op map cleanup (the blocking-op ack
      // path above only fires for clear/compact; other SessionEnd
      // reasons could still have a stale blocking-op entry).
      blocking_ops_.erase(agent);

      const auto presence_path =
          cfg_.state_dir + "/presence/" + agent;
      std::error_code ec;
      fs::remove(presence_path, ec);

      // Audit + broker.log so the human (and post-mortems) see when
      // the broker noticed an agent end. Quiet on inbox-ops by
      // design (the monitor's GONE state is enough surface).
      {
        TopicConfig audit;
        audit.name = "audit";
        audit.kind = std::string{kKindAppendLog};
        if (!registry_.contains("audit")) {
          auto _ = registry_.create(audit);
        }
        topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
        topic::SendOpts a_opts;
        a_opts.protocol = "agent-end";
        stampEpoch(a_opts, current_epoch_);
        const auto reason = v->getOrString("reason");
        auto _ = audit_log.append(
            "broker",
            std::format("agent-end agent={} reason={} released={}",
                        agent, reason.empty() ? "(none)" : reason,
                        to_release.size()),
            a_opts);
      }
      continue;
    }

    // /clear wipes context, so the agent's title (the "what is this
    // context window about?" tag set by `bus msg mail --title`) is
    // now stale. Remove the file so monitor's TITLE column reverts
    // to "—" until the next mail with --title sets a new one.
    // /compact preserves a summary of context, so its title stays.
    if (event == "SessionStart") {
      const auto* payload = v->get("payload");
      if (payload != nullptr && payload->isObject() &&
          payload->getOrString("source") == "clear") {
        std::error_code ec;
        fs::remove(cfg_.state_dir + "/title/" + agent, ec);
      }
      continue;
    }

    if (event != "UserPromptSubmit") continue;

    // Find any in-flight record for this agent dispatched before the
    // observed timestamp. ts isn't strictly needed (we accept any
    // UserPromptSubmit after dispatch); broker just acks the
    // oldest-pending-record-for-this-agent. Order is FIFO since
    // in_flight_ stores msg_ids that contain sent_ms as prefix.
    //
    // SKIP records currently held by a blocking-op — those wait for
    // Stop, not UserPromptSubmit.
    std::string oldest_id;
    std::int64_t oldest_dispatch = 0;
    for (const auto& [id, f] : in_flight_) {
      if (f.agent != agent) continue;
      if (blocking_ops_.contains(agent) &&
          blocking_ops_.at(agent) == id) {
        continue;  // wait for Stop, not this UserPromptSubmit
      }
      if (oldest_id.empty() || f.dispatched_at_ms < oldest_dispatch) {
        oldest_id = id;
        oldest_dispatch = f.dispatched_at_ms;
      }
    }
    if (oldest_id.empty()) continue;

    const auto& f = in_flight_.at(oldest_id);
    // Advance the topic's default cursor past the acked record.
    const auto cursor_p =
        topic::cursorPath(cfg_.state_dir, f.topic, "");
    topic::writeCursor(cursor_p, f.cursor_after);
    removeInflight(oldest_id);
    in_flight_.erase(oldest_id);
  }
  events_offset_ = static_cast<std::int64_t>(valid_pos);
}

auto Loop::dispatchAgentInbox(const TopicConfig& cfg) -> void {
  if (cfg.kind != std::string{kKindAgentInbox}) return;

  // Resolve recipient agent from kind_config.
  const auto* agent_v = cfg.kind_config.get("agent");
  if (agent_v == nullptr || !agent_v->isString()) return;
  const std::string agent = agent_v->asString();

  // Off-TTY gate (roadmap 2.1 / transport §5.1). When
  // $STATE/off-tty/<agent> exists, this agent's mail is delivered by its
  // own UserPromptSubmit/SessionStart hook draining via the `drain` RPC
  // and emitting additionalContext — NOT by the broker typing into the
  // pane. Skip the TTY push entirely so the two paths never both deliver
  // the same record. The flag is absent by default, so an unflagged
  // fleet behaves exactly as before — this is the single switch that
  // flips an agent off-TTY, and nothing changes until it's set.
  // (Scoped to agent-inbox mail; tui-commands / slashes still go TTY.)
  {
    struct stat st;
    const auto flag = cfg_.state_dir + "/off-tty/" + agent;
    if (::stat(flag.c_str(), &st) == 0) return;
  }

  // Universal gates: attached, blocking-op in flight.
  if (hasPresenceFile(agent)) return;
  if (blocking_ops_.contains(agent)) return;

  // Pending-blocking-op gate: a /clear or /compact pending at the head
  // of commands-<agent> must run before mail. blocking_ops_ only
  // contains slashes that have ALREADY dispatched; a slash with
  // deliver_when=idle (the bus-slash default) waits on the idle gate
  // in dispatchTuiCommands, and during that wait mail (no idle gate)
  // would otherwise race past it — landing in pre-clear context.
  // Narrow to /clear and /compact: non-blocking slashes don't need
  // ordering relative to mail.
  {
    const std::string commands_topic = "commands-" + agent;
    if (registry_.contains(commands_topic)) {
      const auto cmds_cursor_p =
          topic::cursorPath(cfg_.state_dir, commands_topic, "");
      const auto cmds_cursor = topic::readCursor(cmds_cursor_p);
      const auto cmds_start =
          cmds_cursor > 0
              ? cmds_cursor
              : static_cast<std::int64_t>(topic::kFileHeaderBytes);
      const std::string cmds_log_path =
          cfg_.state_dir + "/topics/" + commands_topic + ".log";
      topic::TopicLog cmds_log{cmds_log_path};
      if (auto p = cmds_log.peek(cmds_start, 1); p && !p->empty()) {
        const auto& m = (*p)[0];
        if (m.body == "/clear" || m.body == "/compact" ||
            m.body.starts_with("/clear ") ||
            m.body.starts_with("/compact ")) {
          std::println(stderr,
                       "delivery: defer agent-inbox dispatch for {} "
                       "(blocking-op /{} pending on commands-{})",
                       agent, (m.body.starts_with("/compact") ? "compact" : "clear"),
                       agent);
          std::fflush(stderr);  // stderr is block-buffered when redirected
          return;
        }
      }
    }
  }

  const auto cursor_p =
      topic::cursorPath(cfg_.state_dir, cfg.name, "");
  const auto cursor = topic::readCursor(cursor_p);
  const auto start =
      cursor > 0 ? cursor
                 : static_cast<std::int64_t>(topic::kFileHeaderBytes);

  const std::string log_path =
      cfg_.state_dir + "/topics/" + cfg.name + ".log";
  topic::TopicLog log{log_path};
  auto r = log.peek(start, 4);
  if (!r || r->empty()) return;

  // Pick the first record that isn't expired AND isn't already
  // in-flight AND passes the deliver_when gate. We dispatch ONE per
  // tick per topic; the next tick can pick up the next.
  const auto now = nowMs();
  for (const auto& m : *r) {
    // TTL.
    if (m.ttl_ms != 0 && m.sent_ms + static_cast<std::int64_t>(m.ttl_ms) <
                              now) {
      // Expired — advance cursor past, no delivery.
      topic::writeCursor(cursor_p, m.next_offset);
      continue;
    }
    // Epoch quarantine. A record stamped with a different epoch
    // belongs to a previous broker run (or wasn't stamped at all,
    // i.e. epoch 0 from a legacy producer or a pre-feature record).
    // Escalate via the audit + inbox-ops trail and advance the cursor
    // — never deliver. This is the resilience hook that lets the
    // wipe-on-boot shrink to just session-volatile state without
    // re-delivering yesterday's mail as new.
    if (const auto rec_epoch = recordEpoch(m);
        rec_epoch != current_epoch_) {
      InFlight fake;
      fake.msg_id = m.id;
      fake.topic = cfg.name;
      fake.agent = agent;
      fake.cursor_after = m.next_offset;
      fake.dispatched_at_ms = now;
      escalate(fake,
               std::format("stale-epoch (record={}, current={})",
                           rec_epoch, current_epoch_),
               m.body);
      topic::writeCursor(cursor_p, m.next_offset);
      continue;
    }
    // Already in-flight? Wait for ack.
    if (in_flight_.contains(m.id)) return;

    // deliver_when=idle gate. "Idle" means: agent is at the prompt,
    // ready to receive input. Two paths qualify:
    //   1. State::Idle — computed from events + state machine.
    //   2. State::Starting + pane in INSERT — broker has no event
    //      history for this agent (fresh boot, post-wipe) but the
    //      pane is visibly at the prompt; trust the TTY.
    if (m.deliver_when == 1) {
      const std::string events_log = cfg_.state_dir + "/events.jsonl";
      std::set<std::string> filter{agent};
      auto agents = readAgents(events_log, filter);
      AgentInfo info;
      if (auto it = agents.find(agent); it != agents.end()) info = it->second;
      const auto pane = paneState(agent);
      const auto st = computeState(info, 0, now, pane.ok);
      const bool agent_ready =
          st == State::Idle ||
          (st == State::Starting && pane.ok && pane.mode == "INSERT");
      if (!agent_ready) return;
    }

    // Build payload — inline for small, pointer for large.
    std::string payload;
    if (m.body.size() <= kInlineMaxBytes) {
      payload = formatInlineBody(m);
    } else {
      // Materialize payload file.
      std::error_code ec;
      fs::create_directories(payloadsDir(cfg_), ec);
      const auto path = payloadsDir(cfg_) + "/" + m.id + ".body";
      const std::string tmp = path + ".tmp";
      {
        std::ofstream out{tmp};
        out << m.body;
      }
      ::rename(tmp.c_str(), path.c_str());
      payload = formatPointerBody(m);
    }

    if (!deliverInline(cfg_, agent, payload)) {
      // Write failed (pane gone, zellij absent, etc.). Retry on the
      // next tick; do not mark in-flight.
      return;
    }

    // Track in-flight (cursor advances only on ack).
    InFlight f;
    f.msg_id = m.id;
    f.topic = cfg.name;
    f.agent = agent;
    f.dispatched_at_ms = now;
    f.cursor_after = m.next_offset;
    f.attempts = 1;
    f.next_retry_at = now + ackTimeoutMs();
    in_flight_.insert_or_assign(m.id, f);
    writeInflight(f);
    return;  // one dispatch per tick per topic
  }
}

auto Loop::dispatchTuiCommands(const TopicConfig& cfg) -> void {
  if (cfg.kind != std::string{kKindTuiCommands}) return;

  const auto* agent_v = cfg.kind_config.get("agent");
  if (agent_v == nullptr || !agent_v->isString()) return;
  const std::string agent = agent_v->asString();

  // Universal gates: attached, blocking-op in flight.
  if (hasPresenceFile(agent)) return;
  if (blocking_ops_.contains(agent)) return;

  const auto cursor_p =
      topic::cursorPath(cfg_.state_dir, cfg.name, "");
  const auto cursor = topic::readCursor(cursor_p);
  const auto start =
      cursor > 0 ? cursor
                 : static_cast<std::int64_t>(topic::kFileHeaderBytes);

  const std::string log_path =
      cfg_.state_dir + "/topics/" + cfg.name + ".log";
  topic::TopicLog log{log_path};
  auto r = log.peek(start, 4);
  if (!r || r->empty()) return;

  const auto now = nowMs();
  for (const auto& m : *r) {
    if (m.ttl_ms != 0 && m.sent_ms + static_cast<std::int64_t>(m.ttl_ms) <
                              now) {
      topic::writeCursor(cursor_p, m.next_offset);
      continue;
    }
    // Epoch quarantine — see dispatchAgentInbox for the rationale.
    if (const auto rec_epoch = recordEpoch(m);
        rec_epoch != current_epoch_) {
      InFlight fake;
      fake.msg_id = m.id;
      fake.topic = cfg.name;
      fake.agent = agent;
      fake.cursor_after = m.next_offset;
      fake.dispatched_at_ms = now;
      escalate(fake,
               std::format("stale-epoch (record={}, current={})",
                           rec_epoch, current_epoch_),
               m.body);
      topic::writeCursor(cursor_p, m.next_offset);
      continue;
    }
    if (in_flight_.contains(m.id)) return;

    // tui-commands records default to deliver_when=idle (set by
    // `bus msg slash`). Same gate as dispatchAgentInbox: Idle, or
    // Starting+INSERT for post-wipe boots with no event history.
    if (m.deliver_when == 1) {
      const std::string events_log = cfg_.state_dir + "/events.jsonl";
      std::set<std::string> filter{agent};
      auto agents = readAgents(events_log, filter);
      AgentInfo info;
      if (auto it = agents.find(agent); it != agents.end()) info = it->second;
      const auto pane = paneState(agent);
      const auto st = computeState(info, 0, now, pane.ok);
      const bool agent_ready =
          st == State::Idle ||
          (st == State::Starting && pane.ok && pane.mode == "INSERT");
      if (!agent_ready) return;
    }

    // Run the dispatch state machine. Synchronous — can take up to
    // ~5s on retries. Only this topic stalls; other topics still
    // dispatch on subsequent ticks.
    if (!dispatch::dispatchTui(cfg_, agent, m.body)) {
      return;  // dispatch failed (pane gone or stuck); retry next tick
    }

    // Track in-flight (cursor advances on Stop event for blocking
    // ops; on UserPromptSubmit for non-blocking slashes).
    InFlight f;
    f.msg_id = m.id;
    f.topic = cfg.name;
    f.agent = agent;
    f.dispatched_at_ms = now;
    f.cursor_after = m.next_offset;
    f.attempts = 1;
    f.next_retry_at = now + ackTimeoutMs();
    in_flight_.insert_or_assign(m.id, f);
    writeInflight(f);

    // /clear and /compact are blocking-ops: defer all subsequent
    // delivery to this agent until Stop fires. Other slashes are
    // non-blocking — the next UserPromptSubmit acks them.
    if (m.body == "/clear" || m.body == "/compact" ||
        m.body.starts_with("/clear ") || m.body.starts_with("/compact ")) {
      setBlockingOp(agent, m.id);
    }
    return;  // one dispatch per tick per topic
  }
}

auto Loop::escalate(const InFlight& f, std::string_view reason,
                    std::string_view body) -> void {
  // Append to the audit topic (auto-create if absent).
  TopicConfig audit;
  audit.name = "audit";
  audit.kind = std::string{kKindAppendLog};
  if (!registry_.contains("audit")) {
    auto _ = registry_.create(audit);
  }
  topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
  topic::SendOpts opts;
  opts.protocol = "audit";
  // Stamp the broker's own emissions with the current epoch — without
  // this, the very next dispatch tick sees the unstamped audit/ops
  // record as stale, quarantines it, writes another unstamped record,
  // and the broker dispatches itself into an infinite escalation loop.
  stampEpoch(opts, current_epoch_);
  const auto entry = std::format(
      "delivery exhausted msg_id={} topic={} agent={} reason={} body={}",
      f.msg_id, f.topic, f.agent, reason, body);
  auto _ = audit_log.append("broker", entry, opts);

  // Mail inbox-ops (auto-creates the topic). The ops inbox carries
  // infrastructure notifications — delivery failures, retry exhaustion,
  // audit escalation — separate from inbox-comms which surfaces real
  // messages directed at the human. See docs/comms.md.
  auto _ig = registry_.getOrAutoCreate("inbox-ops");
  topic::TopicLog ops_log{cfg_.state_dir + "/topics/inbox-ops.log"};
  topic::SendOpts mopts;
  mopts.protocol = "delivery-failure";
  stampEpoch(mopts, current_epoch_);
  const auto mail = std::format(
      "delivery to {} exhausted ({}): {}", f.agent, reason, body);
  auto _ig2 = ops_log.append("broker", mail, mopts);
}

auto Loop::scanRetries() -> void {
  const auto now = nowMs();
  // Snapshot the in-flight ids that are due; mutate the map as we go.
  std::vector<std::string> due;
  for (const auto& [id, f] : in_flight_) {
    if (f.next_retry_at <= 0) continue;
    if (now < f.next_retry_at) continue;
    // Skip records held by a blocking-op — those wait for Stop, no
    // timer-driven retry.
    if (blocking_ops_.contains(f.agent) &&
        blocking_ops_.at(f.agent) == id) {
      continue;
    }
    due.push_back(id);
  }

  for (const auto& id : due) {
    auto& f = in_flight_.at(id);
    // Read the original record off the topic log so we can re-send
    // it. The record sits at offset cursor_after - record_len, but
    // re-walking from cursor is simpler: peek from the topic cursor.
    const auto cursor_p =
        topic::cursorPath(cfg_.state_dir, f.topic, "");
    const auto cursor = topic::readCursor(cursor_p);
    const auto start =
        cursor > 0 ? cursor
                   : static_cast<std::int64_t>(topic::kFileHeaderBytes);
    topic::TopicLog log{cfg_.state_dir + "/topics/" + f.topic + ".log"};
    auto r = log.peek(start, 1);
    if (!r || r->empty() || r->front().id != id) {
      // Record disappeared or cursor moved past; drop the stale
      // in-flight without escalation (this shouldn't happen).
      removeInflight(id);
      in_flight_.erase(id);
      continue;
    }
    const auto& m = r->front();

    if (f.attempts >= kMaxAttempts) {
      // Exhausted — escalate + advance cursor + clear in-flight.
      escalate(f, "no ack after max attempts", m.body);
      topic::writeCursor(cursor_p, m.next_offset);
      if (blocking_ops_.contains(f.agent) &&
          blocking_ops_.at(f.agent) == id) {
        clearBlockingOp(f.agent);
      }
      removeInflight(id);
      in_flight_.erase(id);
      continue;
    }

    // Re-dispatch. For agent-inbox: rewrite the inline body / pointer.
    // For tui-commands: re-run the state machine. Both are idempotent
    // enough that duplicate writes are harmless.
    bool ok = false;
    const auto* tcfg = registry_.get(f.topic);
    if (tcfg != nullptr) {
      if (tcfg->kind == std::string{kKindAgentInbox}) {
        std::string payload;
        if (m.body.size() <= kInlineMaxBytes) {
          payload = formatInlineBody(m);
        } else {
          payload = formatPointerBody(m);
        }
        ok = deliverInline(cfg_, f.agent, payload);
      } else if (tcfg->kind == std::string{kKindTuiCommands}) {
        ok = dispatch::dispatchTui(cfg_, f.agent, m.body);
      }
    }
    f.attempts += 1;
    f.next_retry_at = now + ackTimeoutMs();
    writeInflight(f);
    (void)ok;  // attempt counted whether the write succeeded or not
  }
}

auto Loop::tick() -> void {
  scanEvents();
  scanRetries();
  maybeAutoClear();
  maybeScanTokens();
  maybeWakeIdleOffTty();
  for (const auto& cfg : registry_.list()) {
    if (cfg.kind == std::string{kKindAgentInbox}) dispatchAgentInbox(cfg);
    else if (cfg.kind == std::string{kKindTuiCommands})
      dispatchTuiCommands(cfg);
  }
}

// Auto-clear idle workers. Implements the trigger described in
// docs/context-budget.md §recommendation: agents that have been idle
// past N minutes with nothing pending get a `/clear` enqueued to
// reclaim context. Conservative gates — every clause has to pass:
//
//   - last_event == "Stop"           (mid-task agents not eligible)
//   - idle_minutes >= threshold      (CLAUDE_BUS_AUTO_CLEAR_MIN,
//                                     default 10; 0 disables)
//   - inbox-<agent> empty            (no work queued to pollute the
//                                     clear with stale context)
//   - no in-flight for this agent    (broker hasn't already dispatched
//                                     something else)
//   - not in a blocking op           (prior /clear / /compact still
//                                     resolving)
//   - paneId resolves                (the agent's claude session is
//                                     alive in zellij)
//   - not on the role-exclusion list (comms / primary never auto-
//                                     clear — high continuity)
//   - cooldown elapsed               (we didn't just auto-clear this
//                                     agent)
//
// Idle ≥ 10 min implies cache-cold (5-min TTL), so the cache-TTL gate
// from the doc is automatically satisfied by the idle threshold —
// no separate check needed.
auto Loop::maybeAutoClear() -> void {
  const auto now = nowMs();
  if (now - auto_clear_last_scan_ms_ < 30'000) return;  // every 30 s
  auto_clear_last_scan_ms_ = now;

  // Threshold + opt-out via env. 0 minutes disables auto-clear.
  std::int64_t threshold_min = 10;
  if (const char* env = std::getenv("CLAUDE_BUS_AUTO_CLEAR_MIN");
      env != nullptr && *env != '\0') {
    threshold_min = std::atoll(env);
  }
  if (threshold_min <= 0) return;
  const auto idle_threshold_ms = threshold_min * 60'000;

  // Role-exclusion list. comms and primary hold cross-thread continuity
  // (see clear-policy.md §6); they should only clear under explicit
  // human direction. Anything else is fair game.
  static const std::set<std::string> kSkipRoles{"comms", "primary"};

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});

  for (const auto& [name, info] : agents) {
    if (kSkipRoles.contains(name)) continue;
    if (info.last.event != "Stop") continue;
    if ((now - info.last.ts_ms) < idle_threshold_ms) continue;
    if (now < auto_clear_next_allowed_ms_[name]) continue;
    if (blocking_ops_.contains(name)) continue;

    // Live-pane check — bus state has many ghost markers from old
    // synthetic events that no longer correspond to a pane.
    if (paneId(name).empty()) continue;

    // Inbox depth — peek 1 record from cursor.
    {
      const auto cursor_p = topic::cursorPath(cfg_.state_dir,
                                              "inbox-" + name, "");
      const auto cursor = topic::readCursor(cursor_p);
      const auto start = cursor > 0
                             ? cursor
                             : static_cast<std::int64_t>(
                                   topic::kFileHeaderBytes);
      const std::string log_path =
          cfg_.state_dir + "/topics/inbox-" + name + ".log";
      topic::TopicLog inbox{log_path};
      if (auto r = inbox.peek(start, 1); r && !r->empty()) {
        continue;  // mail pending, defer the clear
      }
    }

    // In-flight for this agent (any topic).
    bool has_in_flight = false;
    for (const auto& [_, f] : in_flight_) {
      if (f.agent == name) {
        has_in_flight = true;
        break;
      }
    }
    if (has_in_flight) continue;

    // All gates pass — enqueue /clear to commands-<agent>.
    const auto commands_topic = "commands-" + name;
    if (auto cr = registry_.getOrAutoCreate(commands_topic); !cr) continue;
    const std::string log_path =
        cfg_.state_dir + "/topics/" + commands_topic + ".log";
    topic::TopicLog cmd_log{log_path};
    topic::SendOpts opts;
    opts.protocol = "auto-clear";
    opts.deliver_when = 1;  // idle
    stampEpoch(opts, current_epoch_);
    auto _ig = cmd_log.append("broker", "/clear", opts);

    // Cool down for 5 minutes so we don't re-enqueue while the clear
    // is still resolving (SessionStart from /clear will overwrite the
    // last_event to non-Stop anyway, but a cooldown is belt-and-
    // suspenders).
    auto_clear_next_allowed_ms_[name] = now + 5 * 60'000;

    // Audit trail. broker.log + audit topic + inbox-ops so the human
    // sees auto-clear actions land in the same place they see
    // delivery failures.
    {
      TopicConfig audit;
      audit.name = "audit";
      audit.kind = std::string{kKindAppendLog};
      if (!registry_.contains("audit")) {
        auto _ = registry_.create(audit);
      }
      topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
      topic::SendOpts a_opts;
      a_opts.protocol = "auto-clear";
      stampEpoch(a_opts, current_epoch_);
      const auto idle_min = (now - info.last.ts_ms) / 60'000;
      auto _ = audit_log.append(
          "broker",
          std::format("auto-clear agent={} idle_min={} threshold_min={}",
                      name, idle_min, threshold_min),
          a_opts);
    }
  }
}

// Doorbell — wake an idle off-TTY agent that has queued mail.
//
// Off-TTY delivery is pull-on-turn: the drain hook fires on
// UserPromptSubmit / SessionStart. A truly IDLE flagged agent (sitting
// at the prompt, taking no turns) would never drain — its mail strands.
// The old TTY push woke an agent because it submitted a prompt; a drain
// does not. So when the loop sees queued mail for an idle, flagged,
// unattended, live agent, it rings a doorbell: a single sentinel submit
// ([bus-wake]) that fires UserPromptSubmit, on which the drain hook
// delivers the mail as additionalContext. The broker writes ONLY the
// sentinel — the mail itself never touches the TTY.
//
// Gates (every clause must pass):
//   - off-TTY flagged ($STATE/off-tty/<agent>) — TTY agents are already
//     woken by the push path itself.
//   - NOT attached (no [bus-attach] sentinel) — never wake a pane the
//     human is occupying.
//   - idle / at the prompt (last event Stop or Notification idle) — a
//     mid-turn agent drains on its own next turn; don't interrupt it.
//   - live pane (paneId resolves).
//   - mail queued past the cursor — nothing to wake for otherwise.
//   - not mid blocking-op; cooldown elapsed (don't re-ring while a wake
//     resolves).
auto Loop::maybeWakeIdleOffTty() -> void {
  const auto now = nowMs();
  if (now - wake_last_scan_ms_ < 5'000) return;  // every 5 s
  wake_last_scan_ms_ = now;

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});

  for (const auto& [name, info] : agents) {
    // Off-TTY flag — the doorbell only rings for pull-delivery agents.
    {
      struct stat st;
      const auto flag = cfg_.state_dir + "/off-tty/" + name;
      if (::stat(flag.c_str(), &st) != 0) continue;
    }
    if (hasPresenceFile(name)) continue;             // human attached
    if (blocking_ops_.contains(name)) continue;
    if (now < wake_next_allowed_ms_[name]) continue;  // cooldown

    // Idle / at the prompt — not mid-turn.
    const bool idle =
        info.last.event == "Stop" ||
        (info.last.event == "Notification" &&
         info.last.notification_type == "idle_prompt");
    if (!idle) continue;

    if (paneId(name).empty()) continue;  // live pane only

    // Mail queued past the inbox cursor?
    {
      const auto cursor_p =
          topic::cursorPath(cfg_.state_dir, "inbox-" + name, "");
      const auto cursor = topic::readCursor(cursor_p);
      const auto start =
          cursor > 0 ? cursor
                     : static_cast<std::int64_t>(topic::kFileHeaderBytes);
      const std::string log_path =
          cfg_.state_dir + "/topics/inbox-" + name + ".log";
      topic::TopicLog inbox{log_path};
      auto r = inbox.peek(start, 1);
      if (!r || r->empty()) continue;  // nothing queued — no wake
    }

    // Ring it: a single sentinel submit via the same flock'd safe-write
    // path mail uses. sendToPaneSafe defers on a scrolled/locked pane,
    // returning false — skip the cooldown so we retry next scan.
    if (!deliverInline(cfg_, name, "[bus-wake]")) continue;
    wake_next_allowed_ms_[name] = now + 30'000;  // 30 s

    // Audit so doorbell rings land where the human sees deliveries.
    {
      TopicConfig audit;
      audit.name = "audit";
      audit.kind = std::string{kKindAppendLog};
      if (!registry_.contains("audit")) {
        auto _ = registry_.create(audit);
      }
      topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
      topic::SendOpts a_opts;
      a_opts.protocol = "doorbell";
      stampEpoch(a_opts, current_epoch_);
      auto _ = audit_log.append(
          "broker",
          std::format("doorbell wake agent={} (off-TTY idle, mail queued)",
                      name),
          a_opts);
    }
  }
}

// Token-scan watcher. Replaces the statusline script's data-write side
// effect (see docs/status-decouple.md). For each live agent, tail its
// transcript JSONL, compute context occupancy from the last assistant
// turn's usage, and write $STATE/status/<agent>.json — the CTX% source
// `bus deck` + `bus monitor` read. Only two fields are consumed
// downstream (used_percentage + context_window_size), so that's all we
// write.
//
// Numerator (context tokens) comes straight from the transcript and
// matches the statusline's context_window.total_input_tokens exactly.
// The denominator (window size) is NOT in the transcript anywhere, so
// it comes from CLAUDE_BUS_CTX_WINDOW (default 200k; the fleet layout
// sets 1M) with a tier-escalation safety net.
auto Loop::maybeScanTokens() -> void {
  const auto now = nowMs();
  if (now - token_scan_last_ms_ < 5'000) return;  // every 5 s
  token_scan_last_ms_ = now;

  std::int64_t configured_window = 200'000;
  if (const char* env = std::getenv("CLAUDE_BUS_CTX_WINDOW");
      env != nullptr && *env != '\0') {
    if (const auto v = std::atoll(env); v > 0) configured_window = v;
  }

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});

  for (const auto& [name, info] : agents) {
    const auto& path = info.last.transcript_path;
    if (path.empty()) continue;
    // Live-pane gate — skip ghost markers from dead sessions.
    if (paneId(name).empty()) continue;

    auto& sc = token_scan_[name];
    // New session (post /clear or /compact gets a fresh session UUID,
    // hence a new transcript path) → re-read from the top.
    if (sc.path != path) {
      sc.path = path;
      sc.offset = 0;
      sc.last_tokens = -1;
    }

    // Incremental tail read: parse only assistant lines appended since
    // the last scan, keeping the most recent turn's occupancy. Mirrors
    // scanEvents' offset + partial-line handling.
    const auto size = fileSize(path);
    if (size > sc.offset) {
      std::ifstream in{path};
      if (in) {
        in.seekg(sc.offset);
        std::string line;
        auto valid = in.tellg();
        while (std::getline(in, line)) {
          if (in.eof()) break;  // partial trailing line — wait for rest
          valid = in.tellg();
          // Cheap substring pre-filter before the JSON parse.
          if (line.find("\"type\":\"assistant\"") == std::string::npos)
            continue;
          if (line.find("\"usage\"") == std::string::npos) continue;
          auto v = json::parse(line);
          if (!v || !v->isObject()) continue;
          const auto* msg = v->get("message");
          if (msg == nullptr || !msg->isObject()) continue;
          const auto* usage = msg->get("usage");
          if (usage == nullptr || !usage->isObject()) continue;
          // Context occupancy = non-output tokens.
          sc.last_tokens =
              usage->getOrInt("input_tokens", 0) +
              usage->getOrInt("cache_creation_input_tokens", 0) +
              usage->getOrInt("cache_read_input_tokens", 0);
        }
        sc.offset = static_cast<std::int64_t>(valid);
      }
    }

    if (sc.last_tokens < 0) continue;  // no assistant turn yet

    // Escalation: if observed tokens exceed the configured window, the
    // agent must be on a larger tier — bump the denominator so we don't
    // report >100%. No-op when the knob already matches the fleet.
    const std::int64_t tier =
        sc.last_tokens > 200'000 ? 1'000'000 : 200'000;
    const auto window =
        configured_window > tier ? configured_window : tier;
    auto pct = (sc.last_tokens * 100 + window / 2) / window;  // rounded
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;

    // Atomic write of the two fields deck + monitor read. The
    // context_window block matches the old statusline projection's
    // shape so the existing scanIntAfter readers keep working.
    const std::string dir = cfg_.state_dir + "/status";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string final_path = dir + "/" + name + ".json";
    const std::string tmp_path =
        final_path + ".tmp." + std::to_string(::getpid());
    {
      std::ofstream out{tmp_path, std::ios::trunc};
      if (!out) continue;
      out << std::format(
          "{{\"agent\":\"{}\",\"ts\":{},\"context_window\":"
          "{{\"used_percentage\":{},\"context_window_size\":{}}}}}\n",
          name, now, pct, window);
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) fs::remove(tmp_path, ec);
  }
}

}  // namespace bus::delivery
