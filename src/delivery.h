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
#include "topic_log.h"
#include "topic_registry.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace bus::delivery {

// Broker epoch helpers. The wire format's `correlation` field is
// declared for RPC pairing but never set today; we repurpose its
// first 8 bytes (little-endian u64) for the broker's boot-epoch.
// Records carrying an epoch that doesn't match the running broker's
// are quarantined on dispatch — see dispatchAgentInbox /
// dispatchTuiCommands.
inline auto stampEpoch(topic::SendOpts& opts, std::uint64_t epoch)
    -> void {
  for (int i = 0; i < 8; ++i) {
    opts.correlation[i] =
        static_cast<std::uint8_t>((epoch >> (i * 8)) & 0xFF);
  }
}

inline auto recordEpoch(const topic::Message& m) -> std::uint64_t {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(m.correlation[i]) << (i * 8);
  }
  return v;
}

constexpr std::size_t kInlineMaxBytes = 1024;

struct InFlight {
  std::string msg_id;
  std::string topic;
  std::string agent;
  std::int64_t dispatched_at_ms{};
  std::int64_t cursor_after{};  // topic-log offset to advance to on ack
  std::int32_t attempts{1};      // re-dispatch count; 1 = first send
  std::int64_t next_retry_at{};  // dispatched_at_ms + kAckTimeoutMs
};

// Tunables. Override via $CLAUDE_BUS_ACK_TIMEOUT_MS for tests so they
// don't have to wait 60 s of real time.
auto ackTimeoutMs() -> std::int64_t;
constexpr std::int32_t kMaxAttempts = 3;

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

 private:
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

  // Per-agent transcript tail state for the token-scan watcher. The
  // watcher reads each live agent's transcript JSONL, computes context
  // occupancy from the last assistant turn's usage, and writes
  // $STATE/status/<agent>.json (the CTX% source deck + monitor read).
  // Offset-based incremental read like scanEvents — only new bytes are
  // parsed. A path change (new session after /clear or /compact) resets
  // the offset to re-read the fresh transcript. See docs/status-decouple.md.
  struct TokenScanState {
    std::string path;
    std::int64_t offset{0};
    std::int64_t last_tokens{-1};
  };
  std::map<std::string, TokenScanState> token_scan_;
  std::int64_t token_scan_last_ms_{0};

  // Doorbell: per-agent cooldown + scan rate-limit for waking idle
  // off-TTY agents that have queued mail (see maybeWakeIdleOffTty).
  std::map<std::string, std::int64_t> wake_next_allowed_ms_;
  std::int64_t wake_last_scan_ms_{0};

  auto scanEvents() -> void;
  auto scanRetries() -> void;
  auto dispatchAgentInbox(const TopicConfig& cfg) -> void;
  auto dispatchTuiCommands(const TopicConfig& cfg) -> void;
  auto escalate(const InFlight& f, std::string_view reason,
                std::string_view body) -> void;
  auto maybeAutoClear() -> void;
  auto maybeScanTokens() -> void;
  auto maybeWakeIdleOffTty() -> void;

  auto writeInflight(const InFlight& f) -> void;
  auto removeInflight(const std::string& msg_id) -> void;
  auto blockingOpPath(std::string_view agent) const -> std::string;
  auto setBlockingOp(std::string_view agent,
                     std::string_view msg_id) -> void;
  auto clearBlockingOp(std::string_view agent) -> void;
  auto loadBlockingOps() -> void;
};

}  // namespace bus::delivery
