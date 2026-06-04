#include "delivery.h"

#include "agent_status.h"
#include "dispatch.h"
#include "event.h"
#include "json_min.h"
#include "pane.h"
#include "retention.h"
#include "tail_reader.h"
#include "topic_log.h"
#include "tty_policy.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Read a whole file into a byte string. Empty on miss/error.
auto readFileBytes(const std::string& path) -> std::string {
  std::ifstream in{path, std::ios::binary};
  if (!in) return {};
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// Atomically replace `path` with `content` (tmp + rename). pid-suffixed
// tmp so two broker instances (shouldn't happen — singleton) can't collide.
auto atomicReplace(const std::string& path, std::string_view content) -> bool {
  const std::string tmp =
      path + ".trim." + std::to_string(::getpid());
  {
    std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
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
  // Recovery ledger first — independent of the in-flight dir, and the early
  // return below (missing in-flight dir on a fresh boot) must not skip it.
  loadRecovery();
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

auto Loop::noteDrainDelivery(const std::string& msg_id,
                             const std::string& topic,
                             const std::string& agent,
                             std::int64_t cursor_after) -> void {
  InFlight f;
  f.msg_id = msg_id;
  f.topic = topic;
  f.agent = agent;
  f.dispatched_at_ms = nowMs();
  f.cursor_after = cursor_after;
  f.attempts = 1;
  f.next_retry_at = 0;  // scanRetries skips (<=0): drain re-delivers, never
                        // TTY-re-dispatch an off-TTY record.
  in_flight_.insert_or_assign(msg_id, f);
  writeInflight(f);
}

auto Loop::scanEvents() -> void {
  const std::string log = cfg_.state_dir + "/events.jsonl";
  bus::TailReader reader{log};

  if (events_offset_ < 0) {
    reader.seekToEnd();  // first tick: skip history, ack only new events
    events_offset_ = reader.offset();
    return;
  }
  reader.setOffset(events_offset_);

  // poll() returns only whole, newline-terminated lines and never
  // advances past a torn trailing record; on a shrink (events.jsonl
  // rotation, see maybeTrimLogs) it resets to the file start. The
  // offset advance + partial-line guard the inline copy hand-rolled
  // now live in one tested component.
  for (auto& line : reader.poll()) {
    if (line.empty()) continue;
    // D7: one typed parse per line (json_min under the hood), shared by
    // every branch — payload.* fields (reason/source/msg_id) come out
    // typed, killing the top-level-vs-payload misreads the flat scanners
    // had (e.g. `reason` lived at payload.reason, never top-level).
    const auto ev = parseEvent(line);
    if (!ev) continue;
    const auto& event = ev->event;
    const auto& agent = ev->agent;
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
         (ev->reason == "clear" || ev->reason == "compact"));
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
        const auto& reason = ev->reason;
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
      if (ev->source == "clear") {
        std::error_code ec;
        fs::remove(cfg_.state_dir + "/title/" + agent, ec);
      }
      continue;
    }

    // bus-ack (D3): the off-TTY drain hook emits {event:bus-ack,
    // payload:{msg_id}} after injecting a record as additionalContext.
    // Ack BY ID — advance that topic's cursor past the record and forget
    // the in-flight entry — replacing the positional UserPromptSubmit
    // join below (which stays for TTY agents that have no drain hook to
    // emit bus-ack). The C2 lastid marker is stamped HERE, at ack-time,
    // not at drain — so a record whose bus-ack never lands re-drains and
    // re-delivers (cursor never moved) without being dedup-skipped.
    if (event == "bus-ack") {
      const auto& msg_id = ev->msg_id;  // payload.msg_id, typed by parseEvent
      if (msg_id.empty()) continue;
      auto it = in_flight_.find(msg_id);
      if (it == in_flight_.end()) continue;  // unknown / already acked
      const auto cursor_p =
          topic::cursorPath(cfg_.state_dir, it->second.topic, "");
      // Monotonic: never move the cursor backwards (out-of-order or
      // duplicate bus-ack from a re-delivery).
      if (topic::readCursor(cursor_p) < it->second.cursor_after) {
        topic::writeCursor(cursor_p, it->second.cursor_after);
      }
      topic::writeLastId(topic::lastIdPath(cursor_p), msg_id);
      removeInflight(msg_id);
      in_flight_.erase(it);
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
  events_offset_ = reader.offset();
}

auto Loop::dispatchAgentInbox(const TopicConfig& cfg) -> void {
  if (cfg.kind != std::string{kKindAgentInbox}) return;

  // Resolve recipient agent from kind_config.
  const auto* agent_v = cfg.kind_config.get("agent");
  if (agent_v == nullptr || !agent_v->isString()) return;
  const std::string agent = agent_v->asString();

  // Off-TTY gate (roadmap 2.1 / transport §5.1). Off-TTY is the FLEET
  // DEFAULT now: every agent's mail is delivered by its own
  // UserPromptSubmit/SessionStart hook draining via the `drain` RPC and
  // emitting additionalContext — NOT by the broker typing into the pane.
  // Skip the TTY push entirely so the two paths never both deliver the
  // same record. The opt-out set (TTY agents) is durable + committed —
  // see tty_policy.h; it is NOT the ephemeral $STATE sentinel that kept
  // getting wiped. (Scoped to agent-inbox mail; tui-commands/slashes
  // still go TTY.)
  if (isOffTty(agent)) return;

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
      const auto pane = paneStateCached(agent);
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
      const auto pane = paneStateCached(agent);
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
  // messages directed at the human.
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

    // An agent-inbox record is in-flight ONLY because deliverInline
    // already SUCCEEDED at dispatch — dispatchAgentInbox returns WITHOUT
    // marking in-flight when the write is deferred — so an in-flight
    // record has already landed on the recipient's pane. Re-delivering it
    // would be a guaranteed duplicate, and on a live TTY a second push
    // appends into the input buffer (garbled / double submit). So for
    // agent-inbox the retry timer is an ACK DEADLINE, not a re-delivery
    // trigger: we only advance the deadline clock (`attempts`) here and
    // let the kMaxAttempts branch above escalate to inbox-ops/audit if no
    // ack ever arrives. Deferred deliveries never reach in-flight, so this
    // loses no legitimate retry.
    //
    // tui-commands DO re-dispatch: their in-flight semantics differ (a
    // not-ready slash that never landed should legitimately retry), so
    // that path keeps re-running the state machine. Slash re-dispatch
    // dedup is tracked as a separate follow-up.
    const auto* tcfg = registry_.get(f.topic);
    if (tcfg != nullptr && tcfg->kind == std::string{kKindTuiCommands}) {
      dispatch::dispatchTui(cfg_, f.agent, m.body);
    }
    f.attempts += 1;  // deadline tick (agent-inbox) / re-dispatch count (tui)
    f.next_retry_at = now + ackTimeoutMs();
    writeInflight(f);
  }
}

auto Loop::tick() -> void {
  scanEvents();
  scanRetries();
  maybeAutoClear();
  maybeAutoRecover();
  maybeScanTokens();
  maybeWakeIdleOffTty();
  maybeTrimLogs();
  for (const auto& cfg : registry_.list()) {
    if (cfg.kind == std::string{kKindAgentInbox}) dispatchAgentInbox(cfg);
    else if (cfg.kind == std::string{kKindTuiCommands})
      dispatchTuiCommands(cfg);
  }
  // D8 Part B: emit any overrun escalations + recompute next_deadline_ms_
  // last, so it reflects post-dispatch in-flight state. The rpc loop reads
  // it via nextDeadlineMs() to arm the timerfd.
  maybeEscalateStuck();
}

// D8 Part B — the escalation deadline source. Emits a one-shot audit alarm
// when a turn (turn_start_ms + stuck budget) or an open tool call
// (open_tool_since_ms + tool budget) overruns, using the fold accumulator
// Part A put on AgentInfo. Recomputes next_deadline_ms_ as the soonest
// STILL-PENDING deadline (turn / tool / in-flight retry); the rpc loop arms
// a timerfd to it so this fires at the deadline even with zero RPC traffic
// (the quiet-fleet claim). The *_alarmed_ sets enforce escalate-once: an
// already-fired condition is excluded from the deadline set, so a never-
// clearing wedge can't pin the timerfd at its 1 ms floor and busy-loop.
//
// Observability ONLY — the recovery action (nudge / clear / respawn) is R1's
// triage table. This run-on-every-tick scan (no rate-limit) is what lets a
// timerfd fire translate into an alarm the instant the budget elapses.
auto Loop::maybeEscalateStuck() -> void {
  const auto now = nowMs();

  std::int64_t stuck_budget = 5 * 60'000;  // turn open this long w/o Stop
  std::int64_t tool_budget = 5 * 60'000;   // tool open this long w/o result
  if (const char* e = std::getenv("CLAUDE_BUS_STUCK_BUDGET_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) stuck_budget = v;
  }
  if (const char* e = std::getenv("CLAUDE_BUS_TOOL_BUDGET_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) tool_budget = v;
  }

  // Live-pane gate keeps a crashed/gone agent's stale fold state from
  // alarming. CLAUDE_BUS_ESCALATE_NO_PANE_GATE disables it for the
  // hermetic timerfd test (no zellij to resolve a pane against).
  const bool gate_on_pane =
      std::getenv("CLAUDE_BUS_ESCALATE_NO_PANE_GATE") == nullptr;

  std::int64_t next = 0;
  auto consider = [&](std::int64_t d) {
    if (d > 0 && (next == 0 || d < next)) next = d;
  };
  auto auditAlarm = [&](std::string_view protocol, const std::string& body) {
    TopicConfig audit;
    audit.name = "audit";
    audit.kind = std::string{kKindAppendLog};
    if (!registry_.contains("audit")) { auto _ = registry_.create(audit); }
    topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
    topic::SendOpts a_opts;
    a_opts.protocol = std::string{protocol};
    stampEpoch(a_opts, current_epoch_);
    auto _ = audit_log.append("broker", body, a_opts);
  };

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});
  for (const auto& [name, info] : agents) {
    // Live pane only — ghost markers from dead sessions don't escalate.
    if (gate_on_pane && paneId(name).empty()) {
      turn_stuck_alarmed_.erase(name);
      tool_wedged_alarmed_.erase(name);
      continue;
    }

    // Turn-stuck: a turn open past budget with no progress.
    if (info.turn_start_ms > 0) {
      const auto dl = info.turn_start_ms + stuck_budget;
      if (now >= dl) {
        if (!turn_stuck_alarmed_.contains(name)) {
          turn_stuck_alarmed_.insert(name);
          auditAlarm("turn-stuck",
                     std::format("turn-stuck agent={} open_ms={} budget_ms={}",
                                 name, now - info.turn_start_ms, stuck_budget));
        }
      } else {
        consider(dl);  // future, still pending
      }
    } else {
      turn_stuck_alarmed_.erase(name);  // turn ended → re-arm next time
    }

    // Tool-wedged: a PreToolUse with no PostToolUse past budget.
    if (!info.open_tool.empty()) {
      const auto dl = info.open_tool_since_ms + tool_budget;
      if (now >= dl) {
        if (!tool_wedged_alarmed_.contains(name)) {
          tool_wedged_alarmed_.insert(name);
          auditAlarm("tool-wedged",
                     std::format("tool-wedged agent={} tool={} open_ms={} "
                                 "budget_ms={}",
                                 name, info.open_tool,
                                 now - info.open_tool_since_ms, tool_budget));
        }
      } else {
        consider(dl);
      }
    } else {
      tool_wedged_alarmed_.erase(name);
    }
  }

  // In-flight TTY retries also stop riding viewer polling: wake at the
  // soonest next_retry_at. Drain (off-TTY) entries carry next_retry_at=0
  // and are skipped by the d>0 guard — they re-deliver via the drain cursor.
  for (const auto& [id, f] : in_flight_) {
    if (f.attempts <= kMaxAttempts) consider(f.next_retry_at);
  }

  next_deadline_ms_ = next;
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
  // When the recovery engine is in soft/on mode it OWNS idle-context clearing
  // (R1, through the breaker/backoff ledger) — stand aside so the agent isn't
  // cleared twice. In observe/off mode (the default) this standalone loop stays
  // the actor, so the default config keeps its exact behavior. See
  // docs/broker-auto-recovery.md §8.
  if (const char* m = std::getenv("CLAUDE_BUS_AUTO_RECOVERY"); m != nullptr) {
    const std::string_view mode{m};
    if (mode == "soft" || mode == "on") return;
  }

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

// Boot load: read every $STATE/recovery/<agent>.json. A boot_id mismatch
// means the machine rebooted since the ledger was written, so its monotonic
// windows are meaningless (steady_clock reset) — discard them and keep a
// fresh ledger tagged with the current boot. Same-boot (broker restart)
// preserves the breaker, which is the whole point.
auto Loop::loadRecovery() -> void {
  namespace fs = std::filesystem;
  const std::string dir = cfg_.state_dir + "/recovery";
  const std::string boot = readBootId();
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".json") continue;
    const auto agent = e.path().stem().string();
    std::ifstream f{e.path()};
    std::stringstream ss;
    ss << f.rdbuf();
    auto parsed = json::parse(ss.str());
    if (!parsed) continue;
    auto l = ledgerFromJson(*parsed);
    if (l.boot_id != boot) {
      l = RecoveryLedger{};  // reboot → windows invalid; reset
      l.boot_id = boot;
    }
    recovery_.insert_or_assign(agent, std::move(l));
  }
}

// Persist one agent's ledger atomically (tmp + rename). Called after any
// breaker/backoff/relaunch-window mutation so the breaker survives a restart.
auto Loop::saveRecovery(const std::string& agent) -> void {
  namespace fs = std::filesystem;
  auto it = recovery_.find(agent);
  if (it == recovery_.end()) return;
  if (it->second.boot_id.empty()) it->second.boot_id = readBootId();
  const std::string dir = cfg_.state_dir + "/recovery";
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = dir + "/" + agent + ".json";
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f{tmp, std::ios::trunc};
    f << json::serialize(ledgerToJson(it->second));
  }
  fs::rename(tmp, path, ec);
}

// P2 auto-recovery triage — Phase A (OBSERVE-ONLY). Evaluates the recovery
// signature table for every live agent and logs what it WOULD do
// ("would-recover ...") to the audit topic, taking NO action. This is the
// false-positive shakedown on the live stream (auri's gate before any action
// ships in Phase B/C). The 2-signals-agree requirement for the would-relaunch
// row lives in its predicate, so the log is honest about what would fire.
//
// Gated by CLAUDE_BUS_AUTO_RECOVERY (default "observe"; "off"/"0" disables).
// Caveat (Phase A): ages are wall-clock-derived event timestamps, so this WILL
// emit suspend/resume false positives — an intentional shakedown signal, and
// exactly why Phase B/C must add the monotonic-clock gate before any action
// fires. The Δtokens/Δt progress signal is consumed from kvothe's token-monitor
// once available; until then transcript staleness is the stand-in. See
// docs/broker-auto-recovery.md.
auto Loop::maybeAutoRecover() -> void {
  const char* mode_env = std::getenv("CLAUDE_BUS_AUTO_RECOVERY");
  const std::string mode = (mode_env != nullptr && *mode_env != '\0')
                               ? mode_env
                               : "observe";
  if (mode == "off" || mode == "0") return;
  // "soft" enables the non-destructive rows (R1 clear); "on" additionally
  // arms relaunch (Phase C). "observe" (default) only logs would-recover and
  // leaves the standalone loops (maybeAutoClear / doorbell) as the actors —
  // so the default config has ZERO behavior change. See §8.
  const bool acting = (mode == "soft" || mode == "on");
  const auto rec_th = recoveryThresholds();

  const auto now = nowMs();
  if (now - auto_recover_last_scan_ms_ < 30'000) return;  // every 30 s
  auto_recover_last_scan_ms_ = now;

  // §6.1a WALL-JUMP GRACE. Event ages are wall-clock (hooks wall-stamp
  // events.jsonl), so a suspend/resume / NTP step / restart-across-reboot
  // inflates every age → false STUCK/idle. steady_clock PAUSES across
  // suspend while wall LEAPS, so Δwall − Δmono between scans is the jump
  // signature. On a jump, arm a grace window during which the engine NO-OPS:
  // every age spanning the jump is untrustworthy until agents emit fresh
  // post-resume events. This is the "recognize-don't-chase" W1 deferred —
  // recovery ACTS, so it's the first consumer that needs it for real.
  const auto mono = nowMonoMs();
  std::int64_t jump_threshold_ms = 5'000;
  if (const char* e = std::getenv("CLAUDE_BUS_CLOCK_JUMP_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) jump_threshold_ms = v;
  }
  std::int64_t grace_ms = 60'000;
  if (const char* e = std::getenv("CLAUDE_BUS_SUSPEND_GRACE_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) grace_ms = v;
  }
  if (last_tick_wall_ms_ > 0) {
    const auto d_wall = now - last_tick_wall_ms_;
    const auto d_mono = mono - last_tick_mono_ms_;
    if (d_wall - d_mono > jump_threshold_ms) {
      suspend_grace_until_mono_ms_ = mono + grace_ms;
      std::println(stderr,
                   "recovery: clock jump (d_wall={}ms d_mono={}ms) — "
                   "suspending auto-recovery for {}ms",
                   d_wall, d_mono, grace_ms);
    }
  }
  last_tick_wall_ms_ = now;
  last_tick_mono_ms_ = mono;
  if (mono < suspend_grace_until_mono_ms_) return;  // in grace → take no action

  // Budgets (env-tunable; reuse the escalation budget where it matches).
  std::int64_t stuck_budget = 5 * 60'000;
  if (const char* e = std::getenv("CLAUDE_BUS_STUCK_BUDGET_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) stuck_budget = v;
  }
  std::int64_t wedge_budget = 5 * 60'000;  // transcript-stale threshold
  if (const char* e = std::getenv("CLAUDE_BUS_WEDGE_BUDGET_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) wedge_budget = v;
  }
  std::int64_t idle_clear_ms = 10 * 60'000;
  if (const char* e = std::getenv("CLAUDE_BUS_AUTO_CLEAR_MIN");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) idle_clear_ms = v * 60'000;
  }

  // would-recover audit logger with a per-(agent,signature) cooldown so a
  // persistently-wedged agent logs once, not every scan. Sets the cooldown
  // only when it actually logs.
  auto logWould = [&](const std::string& agent, std::string_view sig,
                      std::string_view action, const std::string& signals) {
    const std::string key = agent + "\x1f" + std::string{sig};
    if (now < would_recover_next_log_ms_[key]) return;
    would_recover_next_log_ms_[key] = now + 5 * 60'000;  // per-sig cooldown
    TopicConfig audit;
    audit.name = "audit";
    audit.kind = std::string{kKindAppendLog};
    if (!registry_.contains("audit")) { auto _ = registry_.create(audit); }
    topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
    topic::SendOpts opts;
    opts.protocol = "would-recover";
    stampEpoch(opts, current_epoch_);
    auto _ = audit_log.append(
        "broker",
        std::format(
            "would-recover agent={} signature={} action={} signals=[{}]",
            agent, sig, action, signals),
        opts);
  };

  // R1 ACT (soft/on mode): the engine OWNS idle-context clearing, superseding
  // maybeAutoClear (which steps aside at mode≥soft). Gate on the ledger's
  // per-signature backoff, enqueue /clear to commands-<agent> exactly as
  // maybeAutoClear did, then record the fire so backoff arms + persists.
  // Uses the MONOTONIC clock (the ledger's clock). Returns true if it cleared.
  auto recoverClear = [&](const std::string& agent,
                          const std::string& signals) -> bool {
    auto& led = recovery_[agent];
    if (led.boot_id.empty()) led.boot_id = readBootId();
    if (!recoveryDecide(led, "idle-context", RecoveryAction::Clear, mono,
                        rec_th)
             .allow) {
      return false;  // backoff — too soon since the last clear
    }
    const auto commands_topic = "commands-" + agent;
    if (auto cr = registry_.getOrAutoCreate(commands_topic); !cr) return false;
    topic::TopicLog cmd_log{cfg_.state_dir + "/topics/" + commands_topic +
                            ".log"};
    topic::SendOpts opts;
    opts.protocol = "auto-recover-clear";
    opts.deliver_when = 1;  // idle
    stampEpoch(opts, current_epoch_);
    auto _ig = cmd_log.append("broker", "/clear", opts);
    recoveryRecord(led, "idle-context", RecoveryAction::Clear, mono, rec_th);
    saveRecovery(agent);
    // Audit the acted recovery (protocol=recover) — the same place the human
    // sees delivery failures + would-recover rows.
    TopicConfig audit;
    audit.name = "audit";
    audit.kind = std::string{kKindAppendLog};
    if (!registry_.contains("audit")) { auto _ = registry_.create(audit); }
    topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
    topic::SendOpts a;
    a.protocol = "recover";
    stampEpoch(a, current_epoch_);
    auto _ = audit_log.append(
        "broker",
        std::format("recover agent={} signature=idle-context action=clear "
                    "signals=[{}]",
                    agent, signals),
        a);
    return true;
  };

  // Inbox-pending probe (a head record exists past the cursor).
  auto inboxPending = [&](const std::string& agent) -> bool {
    const auto cursor_p =
        topic::cursorPath(cfg_.state_dir, "inbox-" + agent, "");
    const auto cursor = topic::readCursor(cursor_p);
    const auto start = cursor > 0 ? cursor
                                  : static_cast<std::int64_t>(
                                        topic::kFileHeaderBytes);
    topic::TopicLog inbox{cfg_.state_dir + "/topics/inbox-" + agent + ".log"};
    auto r = inbox.peek(start, 1);
    return r && !r->empty();
  };

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});

  for (const auto& [name, info] : agents) {
    // Live pane only; defer if the human is attached or mid blocking-op.
    if (paneId(name).empty()) continue;
    if (hasPresenceFile(name)) continue;
    if (blocking_ops_.contains(name)) continue;

    const bool pending = inboxPending(name);
    bool has_in_flight = false;
    for (const auto& [id, f] : in_flight_) {
      (void)id;
      if (f.agent == name) { has_in_flight = true; break; }
    }
    const auto ax = computeAxes(info, pending ? 1 : 0, now, true);

    // Transcript-staleness (cheap stat; the progress proxy until kvothe's
    // Δtokens/Δt is wired). transcript_age < 0 = unknown (no path / stat fail).
    std::int64_t transcript_age = -1;
    if (!info.last.transcript_path.empty()) {
      struct stat st{};
      if (::stat(info.last.transcript_path.c_str(), &st) == 0) {
        transcript_age = now - static_cast<std::int64_t>(st.st_mtime) * 1000;
      }
    }
    const bool transcript_stale =
        transcript_age >= 0 && transcript_age > wedge_budget;

    // R1 idle-context → clear. comms/primary are excluded exactly as
    // maybeAutoClear excluded them (high cross-thread continuity — clear only
    // on human cue; see clear-policy.md §6). In observe mode this logs what it
    // WOULD do (maybeAutoClear still acts); in soft/on mode the engine acts
    // through the breaker/backoff ledger and maybeAutoClear steps aside.
    static const std::set<std::string> kClearSkip{"comms", "primary"};
    if (ax.turn == TurnAxis::Ready && !pending && !has_in_flight &&
        info.last.event == "Stop" &&
        (now - info.last.ts_ms) >= idle_clear_ms &&
        !kClearSkip.contains(name)) {
      const std::string signals =
          std::format("idle_min={},inbox=empty,in_flight=0",
                      (now - info.last.ts_ms) / 60'000);
      if (!acting || !recoverClear(name, signals)) {
        // observe mode, or backoff blocked the act → log the intent.
        logWould(name, "idle-context", "clear", signals);
      }
    }

    // R2 relaunch-idle → nudge: at a ready prompt with queued mail. This is
    // the DELIVERY doorbell's exact condition (maybeWakeIdleOffTty), which
    // already acts and is NOT mode-gated — so the engine only LOGS here (acting
    // would double-wake). The doorbell owns this action; the row stays for a
    // whole would-recover stream.
    if (ax.turn == TurnAxis::Ready && pending) {
      logWould(name, "relaunch-idle", "nudge",
               std::format("turn=ready,mail=pending,in_flight={}",
                           has_in_flight ? 1 : 0));
    }

    // R3 hung-turn → nudge: an OPEN turn past 2x budget with no transcript
    // progress (fork-free — event + transcript signals only). OBSERVE-ONLY even
    // in soft mode: a nudge here injects into a MID-TURN pane, which is the
    // mid-stream-dropped-turn failure (the streamed text finishes but planned
    // tool calls are dropped). Escalation (maybeEscalateStuck) and relaunch
    // (Phase C, through the breaker) are the safe responses to a hung turn, not
    // a nudge — so this row logs intent and never acts.
    if (info.turn_start_ms > 0 &&
        (now - info.turn_start_ms) > 2 * stuck_budget && transcript_stale) {
      logWould(name, "hung-turn", "nudge",
               std::format("turn_open_ms={},transcript_stale_ms={}",
                           now - info.turn_start_ms, transcript_age));
    }

    // R4 wedged → relaunch: the agent LOOKS busy/stuck by events AND its
    // transcript is stale (cheap, event-only pre-filter — idle agents at a
    // ready prompt are excluded WITHOUT a fork) AND the pane is NOT at an
    // input-ready prompt (the second, independent signal). Two signals must
    // agree — the cardinal relaunch rule; never on transcript-staleness alone.
    // Fork the pane only after the cheap signals match and only when the
    // cooldown would let us log (so a confirmed-wedged agent forks ~once per
    // cooldown, not every scan).
    const bool looks_busy = ax.turn == TurnAxis::Working ||
                            ax.turn == TurnAxis::Stuck ||
                            ax.process == ProcessAxis::Stuck;
    if (transcript_stale && looks_busy) {
      const std::string wkey = name + "\x1f" + std::string{"wedged"};
      if (now >= would_recover_next_log_ms_[wkey]) {
        const auto pane = paneStateCached(name);  // forks zellij — gated above
        const auto ax_pane =
            computeAxes(info, pending ? 1 : 0, now, true, &pane);
        const bool awaiting_input = wakeReadyForMail(ax_pane, &pane) ||
                                    ax_pane.turn == TurnAxis::NeedsInput;
        if (!awaiting_input) {
          logWould(name, "wedged", "relaunch",
                   std::format("transcript_stale_ms={},looks_busy=true,"
                               "pane_awaiting_input=false",
                               transcript_age));
        }
      }
    }

    // R5 thinking-block (#10) needs an error-signature source (TBD); R6
    // context-100% needs P5 output-verification. Both are documented
    // placeholders, not yet evaluated. See docs/broker-auto-recovery.md.
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
// Gates (every clause must pass), plus a strand watchdog:
//   - off-TTY (the default) — TTY agents are woken by the push path.
//   - live pane (paneId resolves) — else gone/transitioning, replays on
//     respawn (not a strand).
//   - mail queued past the inbox cursor — nothing to wake for otherwise.
//   - NOT attached — never wake a pane the human is occupying (attached
//     defer is legitimate, not a strand).
//   - READY at the prompt — computeAxes process=Alive + turn=Ready
//     (Stop, Notification(idle), SessionStart resume/clear — a
//     JUST-RESTARTED agent) OR process=Compacting (SessionStart(compact)
//     is the last event, i.e. /compact just FINISHED and the agent is
//     idle). The gate first missed the restart case, then missed
//     compact — each omission STRANDED a freshly-{relaunched,compacted}
//     agent with queued mail. Excludes booting/working/stuck/needs-input.
//   - not mid blocking-op; cooldown elapsed.
// Strand watchdog: an off-TTY agent whose mail sits undelivered while
// UNATTENDED past CLAUDE_BUS_STRAND_MS (default 120s) emits a one-shot
// audit alarm (protocol=doorbell-strand) — the objective "no mail
// strands silently" signal. Cleared when the mail drains.
auto Loop::maybeWakeIdleOffTty() -> void {
  const auto now = nowMs();
  if (now - wake_last_scan_ms_ < 5'000) return;  // every 5 s
  wake_last_scan_ms_ = now;

  std::int64_t strand_ms = 120'000;
  if (const char* env = std::getenv("CLAUDE_BUS_STRAND_MS");
      env != nullptr && *env != '\0') {
    if (const auto v = std::atoll(env); v > 0) strand_ms = v;
  }

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});

  for (const auto& [name, info] : agents) {
    if (isTtyAgent(name)) continue;  // off-TTY agents only (the default)

    // No live pane → gone/transitioning; the topic-log record replays on
    // respawn. Not a strand — clear tracking.
    const bool pane_exists = !paneId(name).empty();
    if (!pane_exists) {
      mail_queued_since_ms_.erase(name);
      strand_alarmed_.erase(name);
      continue;
    }

    // Mail queued past the inbox cursor?
    bool has_mail = false;
    {
      const auto cursor_p =
          topic::cursorPath(cfg_.state_dir, "inbox-" + name, "");
      const auto cursor = topic::readCursor(cursor_p);
      const auto start =
          cursor > 0 ? cursor
                     : static_cast<std::int64_t>(topic::kFileHeaderBytes);
      topic::TopicLog inbox{cfg_.state_dir + "/topics/inbox-" + name +
                            ".log"};
      auto r = inbox.peek(start, 1);
      has_mail = r && !r->empty();
    }
    if (!has_mail) {  // drained / nothing queued → healthy
      mail_queued_since_ms_.erase(name);
      strand_alarmed_.erase(name);
      continue;
    }

    // Attached → legitimate defer (presence gate), not a strand. Reset
    // the clock so it counts only unattended time.
    if (hasPresenceFile(name)) {
      mail_queued_since_ms_[name] = now;
      continue;
    }

    // Unattended off-TTY agent WITH queued mail. Start the strand clock;
    // alarm once if it overruns — the objective no-stranding signal.
    if (mail_queued_since_ms_[name] == 0) mail_queued_since_ms_[name] = now;
    if (now - mail_queued_since_ms_[name] >= strand_ms &&
        !strand_alarmed_.contains(name)) {
      strand_alarmed_.insert(name);
      const auto queued_ms = now - mail_queued_since_ms_[name];
      TopicConfig audit;
      audit.name = "audit";
      audit.kind = std::string{kKindAppendLog};
      if (!registry_.contains("audit")) { auto _ = registry_.create(audit); }
      topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
      topic::SendOpts a_opts;
      a_opts.protocol = "doorbell-strand";
      stampEpoch(a_opts, current_epoch_);
      auto _ = audit_log.append(
          "broker",
          std::format("doorbell-strand agent={} queued_ms={} — off-TTY "
                      "mail undelivered past threshold",
                      name, queued_ms),
          a_opts);
    }

    // --- wake gates ---
    if (blocking_ops_.contains(name)) continue;
    if (now < wake_next_allowed_ms_[name]) continue;  // cooldown

    // Wakeable at the prompt? wakeReadyForMail covers Alive+Ready (Stop /
    // idle / resume / clear), Compacting (post-/compact idle — the strand
    // fix), AND a fresh-idle boot sitting at an INSERT prompt. That last
    // shape is harness-gap #4: a fresh spawn's only event is
    // SessionStart(startup), which reads Starting/Stuck — event-identical to
    // a wedged boot — so the old Alive-Ready||Compacting gate never rang it
    // and its first brief stranded until a manual nudge. The pane's INSERT
    // mode disambiguates ready-prompt from wedged-modal; a modal boot stays
    // excluded so BOOT_STUCK detection is intact. (Mid-compaction is
    // PreCompact => Working, still excluded.)
    //
    // Cost discipline (broker-wedge fix): paneStateCached() forks a 5 s-capped
    // `zellij dump-screen` on this single delivery-loop thread, so reading a
    // pane for EVERY candidate every scan serializes into tens of seconds
    // under a multi-agent fan-out and starves RPC (saturation wedge, recv-q
    // backs up, never accept()s). The pane is ONLY needed to disambiguate the
    // boot-ambiguous Starting/Stuck states; Alive+Ready / Compacting (the vast
    // majority of idle agents) decide event-only. So classify event-only first
    // and fork a pane read ONLY when boot-ambiguous — established idle agents
    // never fork. Restores the pre-#4 risk profile while keeping fresh-spawn.
    const auto ax0 = computeAxes(info, 0, now, pane_exists);
    if (ax0.process == ProcessAxis::Starting ||
        ax0.process == ProcessAxis::Stuck) {
      const auto pane = paneStateCached(name);  // forks zellij — only here
      if (!wakeReadyForMail(computeAxes(info, 0, now, pane_exists, &pane),
                            &pane)) {
        continue;  // booting but not yet at an INSERT prompt — retry next scan
      }
    } else if (!wakeReadyForMail(ax0, nullptr)) {
      continue;  // not at the prompt yet — retry next scan when ready
    }

    // Ring it: one sentinel submit via the flock'd safe-write path mail
    // uses. sendToPaneSafe defers on a scrolled/locked pane (returns
    // false) — skip the cooldown so we retry next scan.
    if (!deliverInline(cfg_, name, "[bus-wake]")) continue;
    wake_next_allowed_ms_[name] = now + 30'000;  // 30 s

    // Audit each ring — the objective "a wake fired" signal.
    {
      TopicConfig audit;
      audit.name = "audit";
      audit.kind = std::string{kKindAppendLog};
      if (!registry_.contains("audit")) { auto _ = registry_.create(audit); }
      topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
      topic::SendOpts a_opts;
      a_opts.protocol = "doorbell";
      stampEpoch(a_opts, current_epoch_);
      auto _ = audit_log.append(
          "broker",
          std::format("doorbell wake agent={} (off-TTY, idle, mail queued)",
                      name),
          a_opts);
    }
  }
}

// Token-scan watcher. Replaces the statusline script's data-write side
// effect. For each live agent, tail its
// transcript JSONL, compute context occupancy from the last assistant
// turn's usage, and write $STATE/status/<agent>.json — the CTX% source
// `bus monitor` reads. Only two fields are consumed downstream
// (used_percentage + context_window_size), so that's all we write.
//
// Numerator (context tokens) comes straight from the transcript and
// matches the statusline's context_window.total_input_tokens exactly.
// The denominator (window size) is NOT in the transcript anywhere, so
// it comes straight from CLAUDE_BUS_CTX_WINDOW (default 200k; the fleet
// layout sets the real window).
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

    // Incremental tail read via the shared TailReader: parse only
    // assistant lines appended since the last scan, keeping the most
    // recent turn's occupancy. poll() owns the offset advance + torn-tail
    // refusal that this loop used to hand-roll with tellg.
    bus::TailReader reader{path};
    reader.setOffset(sc.offset);
    for (auto& line : reader.poll()) {
      // Cheap substring pre-filter before the JSON parse.
      if (line.find("\"type\":\"assistant\"") == std::string::npos) continue;
      if (line.find("\"usage\"") == std::string::npos) continue;
      auto v = json::parse(line);
      if (!v || !v->isObject()) continue;
      const auto* msg = v->get("message");
      if (msg == nullptr || !msg->isObject()) continue;
      const auto* usage = msg->get("usage");
      if (usage == nullptr || !usage->isObject()) continue;
      // Context occupancy = non-output tokens.
      sc.last_tokens = usage->getOrInt("input_tokens", 0) +
                       usage->getOrInt("cache_creation_input_tokens", 0) +
                       usage->getOrInt("cache_read_input_tokens", 0);
      // Model rides the same assistant turn. Skip <synthetic> turns (they
      // carry no real model) so the column sticks to the last real model.
      if (const auto m = msg->getOrString("model");
          !m.empty() && m != "<synthetic>") {
        sc.last_model = m;
      }
    }
    sc.offset = reader.offset();

    if (sc.last_tokens < 0) continue;  // no assistant turn yet

    // Denominator is just the configured window (CLAUDE_BUS_CTX_WINDOW;
    // the fleet layout sets it to the real window). The old two-tier
    // ">200k ? 1M : 200K" escalation guess is gone — it mis-reported a
    // 1M-window agent sitting under 200k tokens, and the knob is the
    // honest source. pct is clamped to [0,100] so a misconfigured knob
    // can't produce a nonsense number.
    const auto window = configured_window;
    auto pct = (sc.last_tokens * 100 + window / 2) / window;  // rounded
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;

    // Atomic write of the two fields the monitor reads. The
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
      // context_tokens = the RAW occupancy numerator (kvothe's producer
      // contract): un-rounded so P2 can compute a token-granular Δtokens/Δt
      // over its own window (used_percentage's integer %s are too coarse — at
      // a 1M window 1% = 10k tokens, so a slow agent reads as 0% delta).
      // model feeds kvothe's MODEL column. Both pair with the existing ts.
      out << std::format(
          "{{\"agent\":\"{}\",\"ts\":{},\"model\":\"{}\",\"context_tokens\":{},"
          "\"context_window\":"
          "{{\"used_percentage\":{},\"context_window_size\":{}}}}}\n",
          name, now, sc.last_model, sc.last_tokens, pct, window);
    }
    fs::rename(tmp_path, final_path, ec);
    if (ec) fs::remove(tmp_path, ec);
  }
}

// --- Log retention (D1 + D2) --------------------------------------------

// Smallest consumer cursor across every $STATE/cursors/<topic>/*.cursor.
// Returns 0 when no cursor exists (the "nobody has read anything" floor
// that, under the delivery-guarantee clamp, prevents any trim). Defensive
// against the C1 cursor-namespace split: if a topic carries both a
// `_default` and a named-consumer cursor, the laggard wins.
auto Loop::minConsumerCursor(std::string_view topic) const -> std::int64_t {
  const std::string dir =
      cfg_.state_dir + "/cursors/" + std::string{topic};
  std::error_code ec;
  if (!fs::exists(dir, ec)) return 0;
  std::int64_t min_cursor = -1;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".cursor") continue;
    const auto c = topic::readCursor(entry.path().string());
    if (min_cursor < 0 || c < min_cursor) min_cursor = c;
  }
  return min_cursor < 0 ? 0 : min_cursor;
}

auto Loop::rebaseTopicCursors(std::string_view topic,
                              std::int64_t dropped_bytes,
                              std::int64_t header_bytes) -> void {
  const std::string dir =
      cfg_.state_dir + "/cursors/" + std::string{topic};
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".cursor") continue;
    const auto path = entry.path().string();
    const auto c = topic::readCursor(path);
    const auto rebased =
        retention::rebaseCursor(c, dropped_bytes, header_bytes);
    if (rebased != c) topic::writeCursor(path, rebased);
  }
}

// events.jsonl: when it exceeds CLAUDE_BUS_EVENTS_MAX_BYTES, rewrite it
// keeping only the most recent ~half, aligned to a line boundary. The
// retained tail keeps readAgents' per-agent state intact; events_offset_
// jumps to the new EOF so scanEvents does NOT reprocess (and mis-ACK on)
// the retained lines. Advisory log — the canonical binary topic logs are
// untouched. A hook append racing the rename is lost (documented).
auto Loop::trimEventsLog() -> void {
  std::int64_t cap = 16 * 1024 * 1024;
  if (const char* env = std::getenv("CLAUDE_BUS_EVENTS_MAX_BYTES");
      env != nullptr && *env != '\0') {
    cap = std::atoll(env);
  }
  if (cap <= 0) return;  // disabled

  const std::string log = cfg_.state_dir + "/events.jsonl";
  const auto size = fileSize(log);
  if (size <= cap) return;

  const auto bytes = readFileBytes(log);
  if (bytes.empty()) return;
  // Keep the last ~half, snapped forward to the next line boundary.
  std::size_t approx = bytes.size() - static_cast<std::size_t>(cap / 2);
  const auto nl = bytes.find('\n', approx);
  if (nl == std::string::npos) return;  // no boundary past the cut — skip
  const std::size_t cut = nl + 1;
  if (cut >= bytes.size()) return;

  if (!atomicReplace(log, std::string_view{bytes}.substr(cut))) return;
  // Resume the ACK scan at the new EOF — the retained tail was already
  // processed; reprocessing it would positionally ACK newer in-flight.
  events_offset_ = fileSize(log);
}

auto Loop::maybeTrimLogs() -> void {
  const auto now = nowMs();
  // Default 60 s; CLAUDE_BUS_TRIM_INTERVAL_MS lets tests trigger the sweep
  // promptly (mirrors CLAUDE_BUS_ACK_TIMEOUT_MS).
  std::int64_t interval_ms = 60'000;
  if (const char* env = std::getenv("CLAUDE_BUS_TRIM_INTERVAL_MS");
      env != nullptr && *env != '\0') {
    if (const auto v = std::atoll(env); v > 0) interval_ms = v;
  }
  if (now - trim_last_scan_ms_ < interval_ms) return;
  trim_last_scan_ms_ = now;

  trimEventsLog();  // D1

  // D2: per-topic head trim (retention_ms + absolute size cap).
  std::int64_t topic_max_bytes = 8 * 1024 * 1024;
  if (const char* env = std::getenv("CLAUDE_BUS_TOPIC_MAX_BYTES");
      env != nullptr && *env != '\0') {
    topic_max_bytes = std::atoll(env);
  }
  const auto header = static_cast<std::int64_t>(topic::kFileHeaderBytes);

  for (const auto& tc : registry_.list()) {
    const std::int64_t retention_ms = tc.retention_ms;
    if (retention_ms <= 0 && topic_max_bytes <= 0) continue;  // dead config

    const std::string path =
        cfg_.state_dir + "/topics/" + tc.name + ".log";
    topic::TopicLog log{path};
    auto recs = log.dump();
    if (!recs || recs->empty()) continue;

    std::vector<retention::RecordMeta> meta;
    meta.reserve(recs->size());
    for (const auto& m : *recs) {
      meta.push_back({m.offset, m.next_offset, m.sent_ms});
    }

    // Delivery-guaranteed kinds clamp to the consumer floor so undelivered
    // / in-flight mail is never dropped; fire-and-forget kinds expire
    // regardless (lets audit.log, which has no persistent consumer, shrink).
    //
    // CRIT #3: inbox-ops / inbox-human are broker-produced escalation sinks
    // with NO live draining consumer — their consumer cursor sits at the
    // header forever, so the clamp pins retention and they grow unbounded.
    // Exempt them: bound by size/age like a fire-and-forget log (newest
    // kept). The GC reaper still PROTECTS these topics (same {human,ops}
    // reserved set) — we bound the log, never reap the topic.
    const bool reserved_sink =
        tc.name == "inbox-ops" || tc.name == "inbox-human";
    const bool guaranteed = !reserved_sink &&
                            (tc.kind == std::string{kKindAgentInbox} ||
                             tc.kind == std::string{kKindTuiCommands});
    const auto min_cursor = guaranteed ? minConsumerCursor(tc.name) : 0;

    const auto plan =
        retention::planTrim(meta, now, retention_ms, topic_max_bytes, header,
                            min_cursor, guaranteed);
    if (plan.dropped_bytes <= 0) continue;

    // The Log owns the byte rewrite and REPORTS the exact shift it made.
    const auto dropped = log.trimHead(plan.cut_offset);
    if (dropped <= 0) continue;  // no-op / refused — nothing shifted

    // Every absolute offset shifted down by `dropped` — rebase cursors and
    // in-flight trackers for this topic against the Log's reported shift.
    rebaseTopicCursors(tc.name, dropped, header);
    for (auto& [id, f] : in_flight_) {
      if (f.topic != tc.name) continue;
      f.cursor_after = retention::rebaseCursor(f.cursor_after, dropped, header);
      writeInflight(f);
    }

    // Audit the trim (skip when trimming audit itself — that would feed the
    // loop and isn't useful; broker.log via the daemon already records it).
    if (tc.name != "audit") {
      TopicConfig audit;
      audit.name = "audit";
      audit.kind = std::string{kKindAppendLog};
      if (!registry_.contains("audit")) { auto _ = registry_.create(audit); }
      topic::TopicLog audit_log{cfg_.state_dir + "/topics/audit.log"};
      topic::SendOpts a_opts;
      a_opts.protocol = "retention-trim";
      stampEpoch(a_opts, current_epoch_);
      auto _ = audit_log.append(
          "broker",
          std::format("retention-trim topic={} dropped_bytes={} cut_offset={}",
                      tc.name, plan.dropped_bytes, plan.cut_offset),
          a_opts);
    }
  }

  // CRIT #4: evict soft per-agent state for vanished agents on the same 60 s
  // cadence as the log trim — same "GC my own $STATE" sweep.
  pruneDeadAgents();
}

// Erase a vanished agent's SOFT per-agent state (cooldown/alarm maps). These
// only gate logging/wake/clear frequency, so dropping a still-live agent's
// entry merely re-arms it — safe to be slightly aggressive. Never touches
// in_flight_/blocking_ops_ (delivery state, lifecycle-managed elsewhere).
auto Loop::forgetAgent(std::string_view name) -> void {
  const std::string n{name};
  token_scan_.erase(n);
  auto_clear_next_allowed_ms_.erase(n);
  wake_next_allowed_ms_.erase(n);
  mail_queued_since_ms_.erase(n);
  strand_alarmed_.erase(n);
  turn_stuck_alarmed_.erase(n);
  tool_wedged_alarmed_.erase(n);
  // would_recover_next_log_ms_ is keyed by "<agent>\x1f<sig>" — drop every
  // signature entry for this agent.
  const std::string prefix = n + "\x1f";
  std::erase_if(would_recover_next_log_ms_, [&](const auto& kv) {
    return kv.first.starts_with(prefix);
  });
}

// Prune soft per-agent state for agents no longer present in the events log.
// events.jsonl is retention-bounded (D1), so a long-despawned dynamic peer
// ages out of readAgents and its cooldown/alarm entries are reclaimed.
auto Loop::pruneDeadAgents() -> void {
  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  const auto live = readAgents(events_log, {});
  const auto isDead = [&](const std::string& agent) {
    return !live.contains(agent);
  };
  // Collect dead agents across the maps, then forget each once.
  std::set<std::string> dead;
  for (const auto& [k, _] : token_scan_) if (isDead(k)) dead.insert(k);
  for (const auto& [k, _] : auto_clear_next_allowed_ms_) if (isDead(k)) dead.insert(k);
  for (const auto& [k, _] : wake_next_allowed_ms_) if (isDead(k)) dead.insert(k);
  for (const auto& [k, _] : mail_queued_since_ms_) if (isDead(k)) dead.insert(k);
  for (const auto& k : strand_alarmed_) if (isDead(k)) dead.insert(k);
  for (const auto& k : turn_stuck_alarmed_) if (isDead(k)) dead.insert(k);
  for (const auto& k : tool_wedged_alarmed_) if (isDead(k)) dead.insert(k);
  for (const auto& [k, _] : would_recover_next_log_ms_) {
    const auto sep = k.find('\x1f');
    const auto agent = sep == std::string::npos ? k : k.substr(0, sep);
    if (isDead(agent)) dead.insert(agent);
  }
  for (const auto& agent : dead) forgetAgent(agent);
}

}  // namespace bus::delivery
