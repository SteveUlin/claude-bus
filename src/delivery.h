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
#include "topic_registry.h"

#include <cstdint>
#include <map>
#include <string>

namespace bus::delivery {

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
  Loop(const BrokerConfig& cfg, TopicRegistry& registry);

  // Read in-flight files from disk on broker startup. Idempotent.
  auto load() -> void;

  // One tick: scan events.jsonl for acks, then dispatch new records.
  // Called from rpc::Server::run's tick callback.
  auto tick() -> void;

  // Public for tests / debugging.
  auto inFlight() const -> const std::map<std::string, InFlight>& {
    return in_flight_;
  }

 private:
  const BrokerConfig& cfg_;
  TopicRegistry& registry_;
  std::map<std::string, InFlight> in_flight_;  // keyed by msg_id

  // events.jsonl tail position. Each tick reads from here to EOF and
  // advances. -1 means "seek to end on first tick" (we don't ack old
  // events from before broker startup).
  std::int64_t events_offset_{-1};

  // Agents currently in a blocking op (mid /clear or /compact).
  // Delivery to that agent is deferred until the next Stop event.
  std::map<std::string, std::string> blocking_ops_;  // agent → msg_id

  auto scanEvents() -> void;
  auto scanRetries() -> void;
  auto dispatchAgentInbox(const TopicConfig& cfg) -> void;
  auto dispatchTuiCommands(const TopicConfig& cfg) -> void;
  auto escalate(const InFlight& f, std::string_view reason,
                std::string_view body) -> void;

  auto writeInflight(const InFlight& f) -> void;
  auto removeInflight(const std::string& msg_id) -> void;
  auto blockingOpPath(std::string_view agent) const -> std::string;
  auto setBlockingOp(std::string_view agent,
                     std::string_view msg_id) -> void;
  auto clearBlockingOp(std::string_view agent) -> void;
  auto loadBlockingOps() -> void;
};

}  // namespace bus::delivery
