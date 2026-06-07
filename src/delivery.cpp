#include "delivery.h"

#include "agent_status.h"
#include "blackboard_actor.h"
#include "dispatch.h"
#include "dispatch_actor.h"
#include "envelope.h"
#include "event.h"
#include "journal.h"
#include "json_min.h"
#include "pane.h"
#include "recovery.h"
#include "recovery_actor.h"
#include "tail_reader.h"
#include "tty_policy.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
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
auto formatInlineBody([[maybe_unused]] const bus::Record& r,
                      const bus::msg::Envelope& env) -> std::string {
  std::string out;
  out += "## bus msg mail from ";
  out += env.sender;
  out += " [";
  out += env.protocol;
  out += "]";
  if (!env.body.empty()) {
    out += '\n';
    out += env.body;
  }
  return out;
}

// Build the pointer payload for large bodies. The recipient pulls the
// body through the bus API rather than slurping it into context. Use
// `bus msg body MSG_ID` (side-effect-free) instead of `bus msg fetch` so the
// read doesn't self-ack and bypass the broker's retry/escalation
// safety net.
auto formatPointerBody(const bus::Record& r,
                       const bus::msg::Envelope& env) -> std::string {
  return std::format(
      "## bus msg mail from {} [{}] — large; read with: bus msg body {}",
      env.sender, env.protocol, r.id);
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
  // Register the P2 auto-recovery Policy actor first — it loads its ledger in
  // its constructor (independent of the in-flight dir, and the early return
  // below on a fresh boot must not skip it). The observe/soft/on mode is a
  // runtime decision read here and handed to the actor; the engine never sees
  // it. Add future autonomous behaviors as additional registerActor calls.
  const char* mode_env = std::getenv("CLAUDE_BUS_AUTO_RECOVERY");
  const std::string mode = (mode_env != nullptr && *mode_env != '\0')
                               ? mode_env
                               : "observe";
  engine_.registerActor(
      std::make_unique<RecoveryActor>(cfg_.state_dir, mode));
  // M2: the work-queue dispatcher. Inert until a work-queue topic exists (empty
  // queue_head → no assignments), so registering it is behavior-neutral on a
  // fleet with no work-queue. See docs/work-queue-dispatch.md.
  engine_.registerActor(std::make_unique<DispatchActor>());
  // M2 second pattern: the blackboard fan-out notifier. Also inert until a
  // blackboard topic exists. See docs/blackboard-notify.md.
  engine_.registerActor(std::make_unique<BlackboardActor>());
  // Read every $STATE/in-flight/*.json into memory. Survives restart.
  in_flight_.clear();
  const auto dir = inflightDir(cfg_);
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".json") continue;  // skip .tmp / partials
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
    f.cursor_after = bus::Journal::cursorFromToken(
        v->getOrString("cursor_after"));
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
  obj.insert({"cursor_after", json::Value::from(
                  bus::Journal::cursorToToken(f.cursor_after))});
  obj.insert({"attempts", json::Value::from(static_cast<std::int64_t>(
                              f.attempts))});
  obj.insert({"next_retry_at", json::Value::from(f.next_retry_at)});
  const auto wire = json::serialize(json::Value::fromObject(std::move(obj)));
  const std::string tmp = path + ".tmp";
  std::ofstream out{tmp};
  out << wire;
  out.close();
  if (out) {
    ::rename(tmp.c_str(), path.c_str());
  } else {
    fs::remove(tmp, ec);  // don't leave a partial tmp behind on write failure
  }
}

auto Loop::removeInflight(const std::string& msg_id) -> void {
  std::error_code ec;
  fs::remove(inflightPath(cfg_, msg_id), ec);
  // Drop the materialized large-body payload, if any — the single terminal
  // cleanup point: every real in-flight removal (onAck → forgetInflight,
  // the drop RPC, SessionEnd release, scanRetries TTL/max-attempts) routes
  // through here, so $STATE/payloads/<id>.body can't leak. No-op for inline
  // bodies (no payload file). The read path (bus msg body) never deletes, so a
  // delete only fires once the record is terminally done (acked / dropped /
  // released), by which point the recipient has had its turn to read it.
  fs::remove(payloadsDir(cfg_) + "/" + msg_id + ".body", ec);
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

auto Loop::onAck(const std::string& msg_id, bool write_lastid) -> bool {
  auto it = in_flight_.find(msg_id);
  if (it == in_flight_.end()) return false;  // unknown / already acked
  const auto& f = it->second;
  bus::Journal journal{cfg_.state_dir, f.topic};
  journal.ack("", f.cursor_after, write_lastid ? msg_id : "");
  forgetInflight(msg_id);  // removes file + erases map + clears blocking-op
  return true;
}

auto Loop::noteDrainDelivery(const std::string& msg_id,
                             const std::string& topic,
                             const std::string& agent,
                             Journal::Cursor cursor_after) -> void {
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
      // The blocking-op msg_id is also tracked as in-flight (the dispatch
      // wrote it there). onAck advances its cursor + forgets it (forgetInflight
      // clears the matching blocking-op); clearBlockingOp after covers the case
      // where the record already left in-flight (onAck no-ops then).
      onAck(blocking_ops_.at(agent), /*write_lastid=*/false);
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
        bus::Journal audit_log{cfg_.state_dir + "/topics/audit.log"};
        bus::msg::Envelope a_env;
        a_env.sender = "broker";
        a_env.protocol = "agent-end";
        stampEpoch(a_env, current_epoch_);
        const auto& reason = ev->reason;
        a_env.body = std::format("agent-end agent={} reason={} released={}",
                                 agent, reason.empty() ? "(none)" : reason,
                                 to_release.size());
        auto _ = audit_log.append(bus::msg::encodeEnvelope(a_env));
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
      // Monotonic advance + lastid stamp + forget, all via onAck (the lastid
      // marker is written at ack-time so a record whose bus-ack never lands
      // re-drains without being dedup-skipped). No-ops if already acked.
      onAck(msg_id, /*write_lastid=*/true);
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
    // Positional ack (TTY agents, no drain hook): advance + forget via onAck.
    onAck(oldest_id, /*write_lastid=*/false);
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
      bus::Journal cmds_log{cfg_.state_dir, commands_topic};
      if (auto p = cmds_log.peek(cmds_log.consumerCursor(""), 1);
          p && !p->empty()) {
        const auto& rec = (*p)[0];
        const auto cmds_env = bus::msg::decodeEnvelope(rec.payload);
        if (cmds_env.body == "/clear" || cmds_env.body == "/compact" ||
            cmds_env.body.starts_with("/clear ") ||
            cmds_env.body.starts_with("/compact ")) {
          std::println(stderr,
                       "delivery: defer agent-inbox dispatch for {} "
                       "(blocking-op /{} pending on commands-{})",
                       agent, (cmds_env.body.starts_with("/compact") ? "compact" : "clear"),
                       agent);
          std::fflush(stderr);  // stderr is block-buffered when redirected
          return;
        }
      }
    }
  }

  bus::Journal log{cfg_.state_dir, cfg.name};
  auto r = log.peek(log.consumerCursor(""), 4);
  if (!r || r->empty()) return;

  // Pick the first record that isn't expired AND isn't already
  // in-flight AND passes the deliver_when gate. We dispatch ONE per
  // tick per topic; the next tick can pick up the next.
  const auto now = nowMs();
  for (const auto& rec : *r) {
    // Already in-flight? Wait for ack — BEFORE the TTL/epoch checks below, so a
    // TTL-elapsed (or stale-epoch) record that is ALSO in-flight can't have its
    // cursor advanced past it here. Advancing past an in-flight record would
    // double-dispatch it and leave the cursor unreconcilable with the in-flight
    // set; the in-flight machinery (ack / retry / escalate) owns its lifecycle.
    if (in_flight_.contains(rec.id)) return;
    // Decode envelope once for this record — TTL/epoch/deliver_when/body all live
    // in the payload; decoding once here is cheaper than multiple per-field calls.
    const auto env = bus::msg::decodeEnvelope(rec.payload);
    // TTL.
    if (env.ttl_ms != 0 && rec.append_ms + static_cast<std::int64_t>(env.ttl_ms) <
                              now) {
      // Expired — advance cursor past, no delivery. Forward-only guard.
      log.ack("", bus::Journal::cursorAfter(rec));
      continue;
    }
    // Epoch quarantine. A record stamped with a different epoch
    // belongs to a previous broker run (or wasn't stamped at all,
    // i.e. epoch 0 from a legacy producer or a pre-feature record).
    // Escalate via the audit + inbox-ops trail and advance the cursor
    // — never deliver. This is the resilience hook that lets the
    // wipe-on-boot shrink to just session-volatile state without
    // re-delivering yesterday's mail as new.
    if (const auto rec_epoch = recordEpoch(env);
        rec_epoch != current_epoch_) {
      InFlight fake;
      fake.msg_id = rec.id;
      fake.topic = cfg.name;
      fake.agent = agent;
      fake.cursor_after = bus::Journal::cursorAfter(rec);
      fake.dispatched_at_ms = now;
      escalate(fake,
               std::format("stale-epoch (record={}, current={})",
                           rec_epoch, current_epoch_),
               env.body);
      log.ack("", bus::Journal::cursorAfter(rec));
      continue;
    }

    // deliver_when=idle gate. "Idle" means: agent is at the prompt,
    // ready to receive input. Two paths qualify:
    //   1. State::Idle — computed from events + state machine.
    //   2. State::Starting + pane in INSERT — broker has no event
    //      history for this agent (fresh boot, post-wipe) but the
    //      pane is visibly at the prompt; trust the TTY.
    if (env.deliver_when == 1) {
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
    if (env.body.size() <= kInlineMaxBytes) {
      payload = formatInlineBody(rec, env);
    } else {
      // Materialize payload file.
      std::error_code ec;
      fs::create_directories(payloadsDir(cfg_), ec);
      const auto path = payloadsDir(cfg_) + "/" + rec.id + ".body";
      const std::string tmp = path + ".tmp";
      {
        std::ofstream out{tmp};
        out << env.body;
      }
      ::rename(tmp.c_str(), path.c_str());
      payload = formatPointerBody(rec, env);
    }

    if (!deliverInline(cfg_, agent, payload)) {
      // Write failed (pane gone, zellij absent, etc.). Retry on the
      // next tick; do not mark in-flight.
      return;
    }

    // Track in-flight (cursor advances only on ack).
    InFlight f;
    f.msg_id = rec.id;
    f.topic = cfg.name;
    f.agent = agent;
    f.dispatched_at_ms = now;
    f.cursor_after = bus::Journal::cursorAfter(rec);
    f.attempts = 1;
    f.next_retry_at = now + ackTimeoutMs();
    in_flight_.insert_or_assign(rec.id, f);
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

  bus::Journal log{cfg_.state_dir, cfg.name};
  auto r = log.peek(log.consumerCursor(""), 4);
  if (!r || r->empty()) return;

  const auto now = nowMs();
  for (const auto& rec : *r) {
    // In-flight guard FIRST (see dispatchAgentInbox): never advance the cursor
    // past a record that's still in-flight via the TTL/epoch branches below.
    if (in_flight_.contains(rec.id)) return;
    const auto env = bus::msg::decodeEnvelope(rec.payload);
    if (env.ttl_ms != 0 && rec.append_ms + static_cast<std::int64_t>(env.ttl_ms) <
                              now) {
      log.ack("", bus::Journal::cursorAfter(rec));
      continue;
    }
    // Epoch quarantine — see dispatchAgentInbox for the rationale.
    if (const auto rec_epoch = recordEpoch(env);
        rec_epoch != current_epoch_) {
      InFlight fake;
      fake.msg_id = rec.id;
      fake.topic = cfg.name;
      fake.agent = agent;
      fake.cursor_after = bus::Journal::cursorAfter(rec);
      fake.dispatched_at_ms = now;
      escalate(fake,
               std::format("stale-epoch (record={}, current={})",
                           rec_epoch, current_epoch_),
               env.body);
      log.ack("", bus::Journal::cursorAfter(rec));
      continue;
    }

    // tui-commands records default to deliver_when=idle (set by
    // `bus msg slash`). Same gate as dispatchAgentInbox: Idle, or
    // Starting+INSERT for post-wipe boots with no event history.
    if (env.deliver_when == 1) {
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
    if (!dispatch::dispatchTui(cfg_, agent, env.body)) {
      return;  // dispatch failed (pane gone or stuck); retry next tick
    }

    // Track in-flight (cursor advances on Stop event for blocking
    // ops; on UserPromptSubmit for non-blocking slashes).
    InFlight f;
    f.msg_id = rec.id;
    f.topic = cfg.name;
    f.agent = agent;
    f.dispatched_at_ms = now;
    f.cursor_after = bus::Journal::cursorAfter(rec);
    f.attempts = 1;
    f.next_retry_at = now + ackTimeoutMs();
    in_flight_.insert_or_assign(rec.id, f);
    writeInflight(f);

    // /clear and /compact are blocking-ops: defer all subsequent
    // delivery to this agent until Stop fires. Other slashes are
    // non-blocking — the next UserPromptSubmit acks them.
    if (env.body == "/clear" || env.body == "/compact" ||
        env.body.starts_with("/clear ") || env.body.starts_with("/compact ")) {
      setBlockingOp(agent, rec.id);
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
  bus::Journal audit_log{cfg_.state_dir + "/topics/audit.log"};
  bus::msg::Envelope env;
  env.sender = "broker";
  env.protocol = "audit";
  // Stamp the broker's own emissions with the current epoch — without
  // this, the very next dispatch tick sees the unstamped audit/ops
  // record as stale, quarantines it, writes another unstamped record,
  // and the broker dispatches itself into an infinite escalation loop.
  stampEpoch(env, current_epoch_);
  env.body = std::format(
      "delivery exhausted msg_id={} topic={} agent={} reason={} body={}",
      f.msg_id, f.topic, f.agent, reason, body);
  auto _ = audit_log.append(bus::msg::encodeEnvelope(env));

  // Mail inbox-ops (auto-creates the topic). The ops inbox carries
  // infrastructure notifications — delivery failures, retry exhaustion,
  // audit escalation — separate from inbox-comms which surfaces real
  // messages directed at the human.
  auto _ig = registry_.getOrAutoCreate("inbox-ops");
  bus::Journal ops_log{cfg_.state_dir + "/topics/inbox-ops.log"};
  bus::msg::Envelope menv;
  menv.sender = "broker";
  menv.protocol = "delivery-failure";
  stampEpoch(menv, current_epoch_);
  menv.body = std::format(
      "delivery to {} exhausted ({}): {}", f.agent, reason, body);
  auto _ig2 = ops_log.append(bus::msg::encodeEnvelope(menv));
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
    bus::Journal log{cfg_.state_dir, f.topic};
    auto r = log.peek(log.consumerCursor(""), 1);
    if (!r || r->empty() || r->front().id != id) {
      // Record disappeared or cursor moved past; drop the stale
      // in-flight without escalation (this shouldn't happen).
      removeInflight(id);
      in_flight_.erase(id);
      continue;
    }
    const auto& rec = r->front();
    const auto rec_env = bus::msg::decodeEnvelope(rec.payload);

    if (f.attempts >= kMaxAttempts) {
      // Exhausted — escalate + advance cursor + clear in-flight.
      escalate(f, "no ack after max attempts", rec_env.body);
      log.ack("", bus::Journal::cursorAfter(rec));
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
      dispatch::dispatchTui(cfg_, f.agent, rec_env.body);
    }
    f.attempts += 1;  // deadline tick (agent-inbox) / re-dispatch count (tui)
    f.next_retry_at = now + ackTimeoutMs();
    writeInflight(f);
  }
}

auto Loop::tick() -> void {
  updateContinuity();
  scanEvents();
  scanRetries();
  maybeAutoClear();
  runPolicy();
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
    bus::Journal audit_log{cfg_.state_dir + "/topics/audit.log"};
    bus::msg::Envelope a_env;
    a_env.sender = "broker";
    a_env.protocol = std::string{protocol};
    stampEpoch(a_env, current_epoch_);
    a_env.body = body;
    auto _ = audit_log.append(bus::msg::encodeEnvelope(a_env));
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
      bus::Journal inbox{cfg_.state_dir, "inbox-" + name};
      if (auto r = inbox.peek(inbox.consumerCursor(""), 1); r && !r->empty()) {
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
    bus::Journal cmd_log{log_path};
    bus::msg::Envelope cmd_env;
    cmd_env.sender = "broker";
    cmd_env.protocol = "auto-clear";
    cmd_env.deliver_when = 1;  // idle
    cmd_env.body = "/clear";
    stampEpoch(cmd_env, current_epoch_);
    auto _ig = cmd_log.append(bus::msg::encodeEnvelope(cmd_env));

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
      bus::Journal audit_log{cfg_.state_dir + "/topics/audit.log"};
      bus::msg::Envelope a_env;
      a_env.sender = "broker";
      a_env.protocol = "auto-clear";
      stampEpoch(a_env, current_epoch_);
      const auto idle_min = (now - info.last.ts_ms) / 60'000;
      a_env.body = std::format(
          "auto-clear agent={} idle_min={} threshold_min={}",
          name, idle_min, threshold_min);
      auto _ = audit_log.append(bus::msg::encodeEnvelope(a_env));
    }
  }
}

// True iff a record sits past agent's inbox cursor. A pure read fed into the
// PolicyContext snapshot (was an inline lambda in maybeAutoRecover).
auto Loop::inboxPending(const std::string& agent) const -> bool {
  bus::Journal inbox{cfg_.state_dir, "inbox-" + agent};
  auto r = inbox.peek(inbox.consumerCursor(""), 1);
  return r && !r->empty();
}

// Detect a forward clock JUMP (suspend/resume / NTP step) by comparing the wall
// and monotonic deltas between consecutive ticks: steady_clock pauses across
// suspend while wall leaps, so Δwall − Δmono past the threshold is the jump
// signature — and it catches lid-close, which btime/systemBootMs() cannot. The
// broker is the only proc positioned to see it. Records the resume instant as
// continuity_since_ms_; the reporting-truth clamp (kvothe) floors event-age at
// max(systemBootMs(), continuitySinceMs()) so a resume can't flip an agent red
// on staleness alone. This is the loop-plane lift of recovery's §6.1a wall-jump
// signature into a broker-wide signal (recovery keeps its own grace for now;
// de-dup + a transcript-age clamp is a follow-on).
auto Loop::updateContinuity() -> void {
  const auto now = nowMs();
  const auto mono = nowMonoMs();
  std::int64_t jump_threshold_ms = 5'000;
  if (const char* e = std::getenv("CLAUDE_BUS_CLOCK_JUMP_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) jump_threshold_ms = v;
  }
  if (cont_last_wall_ms_ > 0) {
    const auto d_wall = now - cont_last_wall_ms_;
    const auto d_mono = mono - cont_last_mono_ms_;
    if (d_wall - d_mono > jump_threshold_ms) {
      continuity_since_ms_ = now;  // the resume instant
      // Persist for cross-scope readers (the state RPC reads this; viewers may
      // read it directly). An int64 in a file — reuse the cursor codec. Persists
      // across a broker restart, which is correct: a past suspend instant stays
      // a valid event-age floor, and max(systemBootMs(),…) supersedes it after a
      // reboot.
      bus::writeU64File(cfg_.state_dir + "/continuity.ms", now);
      std::println(stderr,
                   "continuity: clock jump (d_wall={}ms d_mono={}ms) — "
                   "continuity_since_ms={}",
                   d_wall, d_mono, now);
    }
  }
  cont_last_wall_ms_ = now;
  cont_last_mono_ms_ = mono;
}

// Build the per-tick PolicyContext (the Readers snapshot + clocks + a lazy pane
// resolver), run every registered actor, and execute the declarative actions
// they return. The snapshot fields are exactly what the inline maybeAutoRecover
// computed per agent; pane acquisition stays here (the loop plane owns the TTL
// cache), pulled lazily via ctx.pane only when an actor's cheap signals match.
auto Loop::runPolicy() -> void {
  policy::PolicyContext ctx;
  ctx.now_wall_ms = nowMs();
  ctx.now_mono_ms = nowMonoMs();
  ctx.pane = [](const std::string& n) { return paneStateCached(n); };

  const std::string events_log = cfg_.state_dir + "/events.jsonl";
  auto agents = readAgents(events_log, {});  // stable for the tick
  for (auto& [name, info] : agents) {
    if (paneId(name).empty()) continue;  // live pane only
    policy::AgentSnapshot s;
    s.name = name;
    s.info = &info;
    s.inbox_pending = inboxPending(name);
    for (const auto& [id, f] : in_flight_) {
      (void)id;
      if (f.agent == name) { s.has_in_flight = true; break; }
    }
    s.attached = hasPresenceFile(name);
    s.blocking_op = blocking_ops_.contains(name);
    s.axes = computeAxes(info, s.inbox_pending ? 1 : 0, ctx.now_wall_ms, true);
    if (!info.last.transcript_path.empty()) {
      struct stat st{};
      if (::stat(info.last.transcript_path.c_str(), &st) == 0) {
        s.transcript_age_ms =
            ctx.now_wall_ms - static_cast<std::int64_t>(st.st_mtime) * 1000;
      }
    }
    ctx.agents.push_back(std::move(s));
  }

  // Fold the head of every work-queue topic into the snapshot so a DispatchActor
  // (pure, reads only ctx) can pair tasks with idle agents. Bounded window — the
  // cost is O(work-queue count), and only the head matters for one-per-tick
  // assignment.
  for (const auto& tcfg : registry_.list()) {
    if (tcfg.kind != std::string{kKindWorkQueue}) continue;
    bus::Journal log{cfg_.state_dir, tcfg.name};
    auto r = log.peek(log.consumerCursor(""), 16);  // bounded head window → batch-assign
    if (!r) continue;
    for (const auto& rec : *r) {
      const auto wq_env = bus::msg::decodeEnvelope(rec.payload);
      ctx.queue_head.push_back({tcfg.name, rec.id, wq_env.body});
    }
  }

  // Fold new blackboard posts (consume-on-read: advance a loop-owned _dispatch
  // cursor as we fold, so a BlackboardActor stays stateless). The cursor file
  // persists, so a restart resumes past already-notified posts.
  for (const auto& tcfg : registry_.list()) {
    if (tcfg.kind != std::string{kKindBlackboard}) continue;
    bus::Journal log{cfg_.state_dir, tcfg.name};
    auto r = log.peek(log.consumerCursor("_dispatch"), 16);  // bounded window of new posts
    if (!r || r->empty()) continue;
    for (const auto& rec : *r) {
      const auto bb_env = bus::msg::decodeEnvelope(rec.payload);
      ctx.board_updates.push_back({tcfg.name, bb_env.sender, bb_env.body});
    }
    // Advance past all records we just folded — forward-only.
    log.ack("_dispatch", bus::Journal::cursorAfter(r->back()));
  }

  for (const auto& a : engine_.evaluate(ctx)) executePolicyAction(a);
}

// Execute one declarative Policy action via the broker's existing paths. The
// only kind emitted in observe/soft is Enqueue (audit rows + the R1 /clear);
// the create-then-append replicates the former logWould / recoverClear lambdas
// exactly (including the explicit append-log create for "audit"), so the audit
// stream is byte-identical. Recover/Nudge are Phase-C (gated) — no-op here.
auto Loop::executePolicyAction(const policy::PolicyAction& a) -> void {
  using Kind = policy::PolicyAction::Kind;
  switch (a.kind) {
    case Kind::Enqueue: {
      if (a.topic == "audit") {
        if (!registry_.contains("audit")) {
          TopicConfig audit;
          audit.name = "audit";
          audit.kind = std::string{kKindAppendLog};
          auto _ = registry_.create(audit);
        }
      } else if (auto cr = registry_.getOrAutoCreate(a.topic); !cr) {
        return;
      }
      bus::Journal log{cfg_.state_dir + "/topics/" + a.topic + ".log"};
      bus::msg::Envelope env;
      env.sender = "broker";
      env.protocol = a.protocol;
      env.deliver_when = a.deliver_when;
      stampEpoch(env, current_epoch_);
      env.body = a.body;
      auto appended = log.append(bus::msg::encodeEnvelope(env));
      // M2 work-queue claim: after the task is durably in the recipient inbox,
      // consume the source queue head (advance its "" cursor past the head),
      // reusing the fetch primitive's semantics. Append-then-consume = if the
      // broker dies between, the task re-assigns (at-least-once) rather than
      // being lost — the broker's delivery ethos. The cursor advance happens
      // HERE in the loop (kernel plane), never in the actor (§1.4). Gate the
      // claim on append success: a failed inbox append must leave the source
      // cursor untouched so the task re-assigns next tick instead of vanishing
      // (the sibling getOrAutoCreate-fail path above returns for the same
      // reason).
      if (appended && !a.consume_from.empty()) consumeQueueHead(a.consume_from);
      break;
    }
    case Kind::Recover:
    case Kind::Nudge:
      break;  // Phase C (gated); not emitted in observe/soft.
  }
}

// Consume one record from a topic's head: advance its "" (shared) cursor past
// the head, exactly as the `fetch` RPC does. The M2 work-queue claim — the loop
// (kernel plane) performs it on a DispatchActor's consume_from intent, so the
// actor never advances a cursor. No-op if the queue is empty.
auto Loop::consumeQueueHead(const std::string& topic) -> void {
  bus::Journal log{cfg_.state_dir, topic};
  auto r = log.peek(log.consumerCursor(""), 1);
  if (!r || r->empty()) return;
  log.ack("", bus::Journal::cursorAfter(r->front()));
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
      bus::Journal inbox{cfg_.state_dir, "inbox-" + name};
      auto r = inbox.peek(inbox.consumerCursor(""), 1);
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
      bus::Journal audit_log{cfg_.state_dir + "/topics/audit.log"};
      bus::msg::Envelope a_env;
      a_env.sender = "broker";
      a_env.protocol = "doorbell-strand";
      stampEpoch(a_env, current_epoch_);
      a_env.body = std::format(
          "doorbell-strand agent={} queued_ms={} — off-TTY "
          "mail undelivered past threshold",
          name, queued_ms);
      auto _ = audit_log.append(bus::msg::encodeEnvelope(a_env));
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
      bus::Journal audit_log{cfg_.state_dir + "/topics/audit.log"};
      bus::msg::Envelope a_env;
      a_env.sender = "broker";
      a_env.protocol = "doorbell";
      stampEpoch(a_env, current_epoch_);
      a_env.body = std::format("doorbell wake agent={} (off-TTY, idle, mail queued)",
                               name);
      auto _ = audit_log.append(bus::msg::encodeEnvelope(a_env));
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

// --- Log trim (D1: events.jsonl only) -----------------------------------

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

// maybeTrimLogs — rate-limited trim of the advisory events.jsonl log (D1).
// Topic log byte offsets are immutable; no per-topic head trim is performed.
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
  // Policy-actor per-agent state (e.g. recovery's would-recover cooldowns) is
  // reclaimed by engine_.pruneDeadAgents in the same sweep — see pruneDeadAgents.
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
  for (const auto& agent : dead) forgetAgent(agent);
  // Policy actors GC their own per-agent state given the live set — the loop
  // can't enumerate their maps (e.g. recovery's would-recover cooldowns).
  std::set<std::string> live_names;
  for (const auto& [k, _] : live) live_names.insert(k);
  engine_.pruneDeadAgents(live_names);
}

}  // namespace bus::delivery
