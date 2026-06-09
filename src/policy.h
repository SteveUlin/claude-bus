#pragma once

// The Policy seam (docs/policy-actors.md): autonomous broker behaviors as
// ACTORS. An actor observes a read-only snapshot, applies its own guards, and
// returns DECLARATIVE actions the loop executes. It never advances a cursor,
// types a pane, or spawns a process — the action vocabulary has no cursor verb,
// so a Policy actor cannot perturb the kernel triad (append-log + cursor-
// advances-on-ack-only + boot-epoch) even by mistake.
//
// The engine never arbitrates: two actors emitting actions on the same target
// in one tick resolve at execution via each topic's existing in-flight / breaker
// / cooldown gating — a double /clear coalesces in tui-commands' in-flight, a
// double Recover dedups in the recovery breaker, exactly as two human-typed
// commands would. Arbitration in the engine would be the god-object creeping
// back, so it is deliberately absent (the engine concatenates in registration
// order and stops there).
//
// This lib (bus_policy) links bus_readers + bus_core ONLY — no pane lib, no
// socket. That link boundary is the structural guard: an actor that reaches for
// pane acquisition or a shared mutable broker field fails to build here.

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "agent_status.h"  // AgentInfo, AgentAxes, computeAxes, wakeReadyForMail
#include "pane_state.h"    // PaneState value type (acquisition stays in the loop)

namespace bus::policy {

// A declarative action an actor intends; the loop ("Router plane") executes it.
// Enqueue is generic over topic and covers the 90 % case (clear, audit,
// escalate, future notes / blackboard posts). Recover (relaunch) + Nudge
// (doorbell) are the two genuinely-distinct mechanisms (process spawn, TTY
// wake), kept NAMED so the audit + the loop see them explicitly. There is no
// cursor-advancing verb — that is the kernel guard, by construction.
struct PolicyAction {
  enum class Kind { Enqueue, Recover, Nudge };
  Kind kind{Kind::Enqueue};
  std::string agent;     // target agent (all kinds)
  std::string topic;     // Enqueue: destination topic
  std::string body;      // Enqueue: record body
  std::string protocol;  // Enqueue: SendOpts.protocol (audit / epoch tag)
  int deliver_when{0};   // Enqueue: SendOpts.deliver_when (0 = now, 1 = idle)
  // Enqueue: if non-empty, the loop ALSO consumes (cursor-advances past) the
  // head of this source topic after the append — the work-queue claim (M2,
  // docs/work-queue-dispatch.md §2 resolution A). The consume is performed by
  // the LOOP (kernel plane, reusing the fetch primitive), never the actor, so
  // §1.4's no-cursor-verb guard holds: an actor expresses intent, the kernel
  // advances the cursor. Empty = a plain Enqueue.
  std::string consume_from;
};

// A task at the head of a work-queue topic, folded into the snapshot by the
// loop plane so a DispatchActor can pair it with an idle agent (it is pure and
// reads only ctx). The component is generic: any pattern that observes a
// topic's head adds its records here (§1.3 "new observation needs are new
// snapshot fields").
struct QueuedTask {
  std::string topic;  // the work-queue topic this task came from
  std::string id;     // record id (head ordering)
  std::string body;   // the task payload
  std::vector<std::string> workers;  // eligible agent names; empty = pool-wide
};

// A new post on a blackboard topic, folded into the snapshot by the loop plane
// (which advances a loop-owned _dispatch cursor as it folds — consume-on-read,
// so a BlackboardActor stays stateless). A fan-out actor notifies every live
// agent except the author. See docs/blackboard-notify.md.
struct BoardUpdate {
  std::string topic;   // the blackboard topic
  std::string author;  // record sender — excluded from the fan-out
  std::string body;    // the post content
};

// The per-(topic,consumer) read snapshot the loop plane builds for every actor.
// Pure values + a LAZY pane resolver: pane acquisition and its TTL cache stay in
// the loop plane (broker-seam-redesign §2); an actor pulls a pane only after its
// cheap signals match (as recovery's R4 does), so the fork cost is never paid
// fleet-wide. Returned BY VALUE — the loop's cache is process-static and the
// actor copies into a local, so no lifetime coupling crosses the seam.
struct AgentSnapshot {
  std::string name;
  const AgentInfo* info{nullptr};      // Readers event-fold (stable for the tick)
  AgentAxes axes;                      // computeAxes WITHOUT pane (cheap)
  std::int64_t transcript_age_ms{-1};  // mtime staleness; -1 = unknown
  bool inbox_pending{false};           // a record sits past the inbox cursor
  bool has_in_flight{false};           // an unacked dispatch for this agent
  bool attached{false};                // hasPresenceFile — human present
  bool blocking_op{false};             // mid /clear or /compact
};

struct PolicyContext {
  std::int64_t now_wall_ms{0};         // hooks wall-stamp events.jsonl (age math)
  std::int64_t now_mono_ms{0};         // ledger / guard clock (suspend-immune)
  std::vector<AgentSnapshot> agents;   // live panes only (paneId resolved)
  std::function<PaneState(const std::string&)> pane;  // lazy, loop-cached
  std::vector<QueuedTask> queue_head;  // head window of work-queue topic(s)
  std::vector<BoardUpdate> board_updates;  // new posts on blackboard topic(s)
};

class PolicyActor {
 public:
  virtual ~PolicyActor() = default;
  virtual auto name() const -> std::string_view = 0;  // audit namespace
  // Decide + guard + record-fire, internally; return the actions for the loop
  // to execute. Pure w.r.t. the broker: no socket, no real time (clocks arrive
  // via ctx), no pane fork beyond ctx.pane. THIS is the litmus surface.
  virtual auto evaluate(const PolicyContext& ctx)
      -> std::vector<PolicyAction> = 0;
  // Reclaim per-agent soft state (cooldowns, etc.) for agents not in `live` —
  // the actor owns its state (§4 leaf guard), so it must also GC it. Driven by
  // the broker's dead-agent prune so dynamic-peer churn can't grow it unbounded.
  // Default no-op; actors with per-agent state override.
  virtual auto pruneDeadAgents(const std::set<std::string>& live) -> void {
    (void)live;
  }
};

class PolicyEngine {
 public:
  // The registration surface. Actors register once at broker startup.
  auto registerActor(std::unique_ptr<PolicyActor> actor) -> void;
  // Fan out over actors in registration order; concatenate their actions. Pure:
  // holds only the actor vector, touches no broker resource.
  auto evaluate(const PolicyContext& ctx) -> std::vector<PolicyAction>;
  // Fan out a dead-agent prune to every actor (each GCs its own per-agent
  // state given the live set).
  auto pruneDeadAgents(const std::set<std::string>& live) -> void;

 private:
  std::vector<std::unique_ptr<PolicyActor>> actors_;
};

}  // namespace bus::policy
