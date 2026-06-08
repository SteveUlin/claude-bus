#pragma once

// Broker delivery loop — runs on each pselect tick from
// src/rpc.{h,cpp}'s server. Walks every agent-inbox topic, evaluates
// gates against the current agent state + presence + in-flight set,
// and dispatches matching records to recipient panes via the same
// per-pane flock the manual `bus msg send` uses.
//
// In-flight records live at $STATE/in-flight/<msg_id>.json. The
// cursor for an agent-inbox topic ONLY advances when the recipient
// ACKs (observed as a UserPromptSubmit event in events.jsonl after
// the dispatch). Until ack, the same record sits at the head and the
// gate skip "already in-flight" prevents re-dispatch.
//
// Phase 4c.2 ships:
//   - DIRECT inline writes (body ≤ kInlineMaxBytes formatted as
//     "## bus msg mail from <sender> [<protocol>]\n<body>")
//   - Large bodies materialized to $STATE/payloads/<id>.body with
//     a pointer line into the pane
//   - Gates: TTL, attached, deliver_when=idle, in-flight
//   - Ack via UserPromptSubmit event observation
//
// Deferred:
//   - Retry on no-ack (4e)
//   - tui-commands dispatch via the dispatch state machine (4d)
//   - pubsub / blackboard / work-queue (4f)

#include "broker.h"
#include "envelope.h"
#include "journal.h"
#include "json_min.h"
#include "policy.h"
#include "token_watcher.h"
#include "topic_registry.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace bus::delivery {

// Broker epoch helpers. The epoch lives in the Envelope (payload layer).
// stampEpoch sets env.epoch before encoding; recordEpoch decodes to read it.
inline auto stampEpoch(bus::msg::Envelope& env, std::uint64_t epoch) -> void {
  env.epoch = epoch;
}

// Decode epoch from a Record's opaque payload.
inline auto recordEpoch(const bus::Record& r) -> std::uint64_t {
  return bus::msg::decodeEnvelope(r.payload).epoch;
}

// For callers that already decoded the envelope.
inline auto recordEpoch(const bus::msg::Envelope& env) -> std::uint64_t {
  return env.epoch;
}

constexpr std::size_t kInlineMaxBytes = 1024;

struct InFlight {
  std::string msg_id;
  std::string topic;
  std::string agent;
  std::int64_t dispatched_at_ms{};
  // Opaque cursor position to advance to on ack. Persisted via
  // Journal::cursorToToken/cursorFromToken — never do arithmetic on it.
  Journal::Cursor cursor_after;
  std::int32_t attempts{1};      // re-dispatch count; 1 = first send
  std::int64_t next_retry_at{};  // dispatched_at_ms + kAckTimeoutMs
};

// Tunables. Override via $CLAUDE_BUS_ACK_TIMEOUT_MS for tests so they
// don't have to wait 60 s of real time.
auto ackTimeoutMs() -> std::int64_t;
constexpr std::int32_t kMaxAttempts = 3;

// P2 auto-recovery is now a Policy actor (RecoveryActor) registered on the
// Loop's PolicyEngine — its ledger, cooldowns, and clock-jump state moved onto
// the actor (the Policy seam; docs/policy-actors.md). recovery.h still provides
// nowMonoMs() to runPolicy and the pure guard types to RecoveryActor.

class Loop {
 public:
  // current_epoch is stamped onto records by the enqueue handler and
  // checked here on dispatch — see broker.cpp's runBroker for how the
  // counter advances per boot.
  Loop(const BrokerConfig& cfg, TopicRegistry& registry,
       std::uint64_t current_epoch);

  // Read in-flight files from disk on broker startup. Idempotent.
  auto load() -> void;

  // One tick: scan events.jsonl for acks, then dispatch new records.
  // Called from rpc::Server::run's tick callback.
  auto tick() -> void;

  // Public for tests / debugging.
  auto inFlight() const -> const std::map<std::string, InFlight>& {
    return in_flight_;
  }

  // Forget any in-flight entry for `msg_id`. Removes the on-disk
  // tracker file AND the in-memory map entry. Used by the `drop` RPC
  // to manually discard a stuck dispatch without firing escalation.
  // No-op when the id isn't in-flight. Returns the entry's
  // {topic, agent, cursor_after} if it was present (for the caller's
  // audit trail), or std::nullopt otherwise.
  auto forgetInflight(const std::string& msg_id) -> std::optional<InFlight>;

  // Register a record delivered via the off-TTY drain pull as in-flight,
  // awaiting a {event:bus-ack,msg_id}. The cursor is NOT advanced here
  // (the ack advances it); next_retry_at is 0 so scanRetries never
  // TTY-re-dispatches it (off-TTY has no pane to type into) — re-delivery
  // happens by the next drain re-reading the un-advanced cursor. Called
  // from the broker's `drain` RPC handler. Idempotent per msg_id.
  auto noteDrainDelivery(const std::string& msg_id, const std::string& topic,
                         const std::string& agent,
                         Journal::Cursor cursor_after) -> void;

  // Soonest pending escalation deadline (absolute ms-since-epoch; 0 =
  // none). The rpc server arms a one-shot timerfd to this after each tick
  // so escalation fires deterministically rather than riding RPC/viewer
  // ticks (D8 Part B). Recomputed by maybeEscalateStuck every tick.
  auto nextDeadlineMs() const -> std::int64_t { return next_deadline_ms_; }

 private:
  // The wall-clock instant (ms) of the most recent detected forward clock JUMP
  // (suspend/resume / NTP step) — the broker is the only proc that can spot a
  // lid-close, which btime/systemBootMs() misses. 0 = no jump seen this run.
  // Persisted to $STATE/continuity.ms; broker reads it back via bus::readU64File.
  // Updated every tick by updateContinuity.
  auto continuitySinceMs() const -> std::int64_t { return continuity_since_ms_; }
  const BrokerConfig& cfg_;
  TopicRegistry& registry_;
  std::uint64_t current_epoch_{0};
  std::map<std::string, InFlight> in_flight_;  // keyed by msg_id

  // events.jsonl tail position. Each tick reads from here to EOF and
  // advances. -1 means "seek to end on first tick" (we don't ack old
  // events from before broker startup).
  std::int64_t events_offset_{-1};

  // Agents currently in a blocking op (mid /clear or /compact).
  // Delivery to that agent is deferred until the next Stop event.
  std::map<std::string, std::string> blocking_ops_;  // agent → msg_id

  // Per-agent timestamp (ms) past which the next auto-clear check is
  // allowed to fire. Prevents re-enqueueing /clear while the previous
  // one is still in flight or while the agent hasn't emitted a new
  // Stop event yet (cooldown). 0 means "no prior auto-clear seen."
  std::map<std::string, std::int64_t> auto_clear_next_allowed_ms_;

  // Last time the periodic auto-clear scan ran. Cheap rate-limit so
  // the scan doesn't fire on every 250 ms broker tick.
  std::int64_t auto_clear_last_scan_ms_{0};

  // P2 auto-recovery is a Policy actor (RecoveryActor) registered on this
  // engine in load(); its ledger + cooldowns + clock-jump state live on the
  // actor. runPolicy() drives the engine each tick. Add future autonomous
  // behaviors as actors here, not as new Loop methods (docs/policy-actors.md).
  policy::PolicyEngine engine_;

  // Last time the events.jsonl trim ran. Rate-limited; see maybeTrimLogs.
  std::int64_t trim_last_scan_ms_{0};

  // Transcript-token scanning, extracted into its own class so the scan
  // concern is off Loop's data surface. scan() is rate-limited internally
  // (5 s); pruneDead() mirrors the forgetAgent sweep for token scan state.
  bus::TokenWatcher token_watcher_;

  // Doorbell: per-agent cooldown + scan rate-limit for waking idle
  // off-TTY agents that have queued mail (see maybeWakeIdleOffTty).
  std::map<std::string, std::int64_t> wake_next_allowed_ms_;
  std::int64_t wake_last_scan_ms_{0};

  // Strand watchdog: per-agent ms when mail was first seen queued +
  // undelivered to an UNATTENDED off-TTY agent (0 = not queued / drained
  // / attached). If it stays past the strand threshold, emit a one-shot
  // alarm (tracked in strand_alarmed_) — the objective "no mail stranded
  // silently" signal. Both reset when the mail drains.
  std::map<std::string, std::int64_t> mail_queued_since_ms_;
  std::set<std::string> strand_alarmed_;

  // D8 Part B escalation deadline source. maybeEscalateStuck emits a
  // one-shot audit alarm when a turn (turn_start_ms + stuck budget) or an
  // open tool call (open_tool_since_ms + tool budget) overruns, and caches
  // next_deadline_ms_ — the soonest still-pending deadline (turn / tool /
  // in-flight retry) the rpc loop arms its timerfd to. The *_alarmed_ sets
  // enforce escalate-once: an already-fired condition drops out of the
  // deadline set, so a never-clearing wedge can't pin the timerfd at the
  // 1 ms floor and busy-loop. Reset when the condition clears.
  std::int64_t next_deadline_ms_{0};
  std::set<std::string> turn_stuck_alarmed_;
  std::set<std::string> tool_wedged_alarmed_;

  // Loop-plane forward-clock-JUMP detection (reporting-truth continuity, the
  // lift of recovery's §6.1a wall-jump signature to a broker-wide signal). Each
  // tick records a (wall, mono) pair; Δwall − Δmono past the threshold marks a
  // suspend/resume / NTP step → record continuity_since_ms_ = the resume instant.
  std::int64_t cont_last_wall_ms_{0};
  std::int64_t cont_last_mono_ms_{0};
  std::int64_t continuity_since_ms_{0};
  auto updateContinuity() -> void;

  auto scanEvents() -> void;
  auto scanRetries() -> void;
  auto dispatchAgentInbox(const TopicConfig& cfg) -> void;
  auto dispatchTuiCommands(const TopicConfig& cfg) -> void;
  auto escalate(const InFlight& f, std::string_view reason,
                std::string_view body) -> void;
  auto maybeAutoClear() -> void;
  // Policy engine driver (replaces the inline maybeAutoRecover): build the
  // per-tick PolicyContext (a Readers snapshot + clocks + a lazy pane
  // resolver), run every registered actor, and execute the declarative actions
  // they return through the existing append paths. The engine never advances a
  // cursor — docs/policy-actors.md.
  auto runPolicy() -> void;
  auto executePolicyAction(const policy::PolicyAction& a) -> void;
  // Consume a topic's head (advance its "" cursor past the head, like `fetch`).
  // The M2 work-queue claim, performed by the loop on a consume_from intent.
  auto consumeQueueHead(const std::string& topic) -> void;
  // True iff a record sits past agent's inbox cursor — a snapshot input for the
  // policy context. Pure read.
  auto inboxPending(const std::string& agent) const -> bool;
  auto maybeWakeIdleOffTty() -> void;
  auto maybeEscalateStuck() -> void;

  // Rate-limited entry from tick(). Trims the advisory events.jsonl log
  // (D1). Byte offsets in topic logs are immutable; no per-topic trim.
  auto maybeTrimLogs() -> void;
  auto trimEventsLog() -> void;
  // Evict a vanished agent's soft per-agent state (cooldown/alarm maps) so
  // P4 dynamic-peer churn can't grow them unbounded. Cooldown/alarm state
  // only, never in-flight/blocking (those are lifecycle-managed). Driven by
  // pruneDeadAgents from the trim sweep.
  auto forgetAgent(std::string_view name) -> void;
  auto pruneDeadAgents() -> void;

  // Append one record to the audit topic. Auto-creates "audit" if absent,
  // opens the journal, constructs an Envelope (sender="broker", epoch-stamped),
  // and appends. Centralises the stampEpoch call — forgetting it causes the
  // infinite-escalation loop warned about at the escalate() call site.
  auto emitAudit(std::string_view protocol, std::string_view body) -> void;

  // True iff `agent` is ready to receive a deliver_when=idle record right now.
  // Reads events.jsonl filtered to the single agent, calls paneStateCached and
  // computeState, and evaluates the idle predicate. Both dispatchAgentInbox and
  // dispatchTuiCommands gate deliver_when=1 records on this.
  auto isAgentIdle(const std::string& agent, std::int64_t now) const -> bool;

  auto writeInflight(const InFlight& f) -> void;
  auto removeInflight(const std::string& msg_id) -> void;

  // ACK an in-flight record by id: monotonically advance its topic's default
  // cursor past the record, optionally stamp the lastid dedup marker, then
  // forget the in-flight entry (which also clears any matching blocking-op).
  // The SOLE path that advances a cursor on an ack — the three ack branches in
  // scanEvents (blocking-op / bus-ack / positional UserPromptSubmit) all route
  // through here, so "what advances a cursor" is one auditable place.
  // No-op returning false when msg_id isn't in-flight (already acked / unknown).
  auto onAck(const std::string& msg_id, bool write_lastid) -> bool;
  auto blockingOpPath(std::string_view agent) const -> std::string;
  auto setBlockingOp(std::string_view agent,
                     std::string_view msg_id) -> void;
  auto clearBlockingOp(std::string_view agent) -> void;
  auto loadBlockingOps() -> void;
};

}  // namespace bus::delivery
