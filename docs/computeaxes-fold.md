# computeAxes as a fold + timerfd escalation (D8)

D8 has two coupled parts. **Part A (the fold)** is bounded, pure, and
behavior-preserving for the existing axes — implemented now, unit-tested.
**Part B (timerfd escalation)** touches `rpc.cpp`'s core pselect reactor and
the meaning of the wall-clock thresholds — **flagged for comms ack before
implementation** (same gate the log-retention.md design went through),
because it changes broker core-loop timing and interacts with a documented
property of the loop (see "Why the timerfd matters" below).

## The root cause D8 targets

`readAgents` keeps only `info.last` — the single most-recent event per
agent — and `computeAxes` derives state as `f(last_event, wall_clock_age)`.
A turn's *structure* is invisible: a `PreToolUse` with no matching
`PostToolUse` (a tool that started and wedged), or a `UserPromptSubmit` that
started a turn the model never advanced (the mid-stream dropped-turn:
streamed text completes, the planned tool call is dropped, the agent sits in
WORKING with an empty prompt) — both look identical to any other mid-turn
event. The only escape is `age_s > 5min → Stuck`, a blunt wall-clock poll.

## Part A — readAgents as a fold (additive, behavior-preserving)

Carry an accumulator across the event sequence instead of overwriting
`last`. New `AgentInfo` fields:

- `turn_start_ms` — ts of the `UserPromptSubmit` that began the current
  turn; `0` when no turn is active (last was `Stop` / idle / boot).
- `open_tool` — a `PreToolUse` tool_name with no matching `PostToolUse`
  yet: a tool call in flight. Empty when none open.
- `open_tool_since_ms` — when that tool call opened.

The fold (per agent, events already in file order):

| event             | accumulator transition                              |
|-------------------|-----------------------------------------------------|
| `UserPromptSubmit`| `turn_start = ts`; clear `open_tool`                |
| `PreToolUse`      | `open_tool = tool_name`; `open_tool_since = ts`     |
| `PostToolUse`     | clear `open_tool` (a tool completed)                |
| `Stop`            | `turn_start = 0`; clear `open_tool` (turn ended)    |
| `Notification(idle_prompt)` | `turn_start = 0` (back at prompt)         |
| `SessionStart`/`SessionEnd` | reset both (new/ended session)            |

`computeAxes`' **axis outputs stay identical** for every input the current
logic already classifies — Part A only *populates* the accumulator and
exposes it on `AgentInfo` (and, downstream, the broker `state` RPC). This
keeps the monitor / deck / agent-bar stable. What the accumulator unlocks:

- a precise diagnostic — "Bash open 7m" vs. a bare "STUCK";
- the input R1's triage table (kvothe) needs to pick an action;
- the deadline the timerfd (Part B) schedules against (`turn_start +
  budget`, `open_tool_since + budget`) instead of a per-tick age poll.

Part A is pure logic in `bus_agent_status`, unit-tested with hand-built
event sequences (turn open/close, tool open/match, dropped-turn, wedged
tool, interleavings).

## Part B — timerfd escalation (decouple from wall-clock) — NEEDS ACK

Today the Working→Stuck (`age_s > 5min`) and boot-stuck (`age_s > 30s`)
transitions are evaluated by recomputing `now - last.ts_ms` *on each tick*.

### Why the timerfd matters (and the risk)

The broker's delivery tick is **RPC-driven in practice** — idle
pselect-timeout ticks don't fire reliably; viewer ~1Hz polling drives the
loop (see the broker-tick-rpc-driven memo / the rpc.cpp inner-budget
comment). So a wall-clock-on-tick escalation only fires *when something else
already woke the loop*. In a quiet fleet (no viewers) a wedged tool or
dropped turn could sit unescalated indefinitely. A `timerfd_create` armed to
the next deadline and added to the pselect `fd_set` makes the loop **wake
exactly when an escalation is due**, independent of RPC traffic — the honest
fix for the same class of bug the retention sweep currently rides viewer
polling to dodge.

**The risk:** this adds an fd + a re-arm step to `rpc::Server::run`'s
reactor (the singleton broker's core loop). It's the highest-blast-radius
surface in the tree. Hence the ack gate.

### Proposed shape (for review, not yet built)

- `rpc::Server::run` gains an optional `timerfd` the tick callback can
  re-arm: after each tick, the delivery loop computes the *earliest* pending
  deadline (nearest of: in-flight retry `next_retry_at`, `turn_start +
  stuck_budget`, `open_tool_since + tool_budget`) and arms the timerfd to
  it. pselect waits on `listen_fd ∪ timerfd`; a timerfd fire is just another
  "run the tick now" wake.
- `computeAxes` keeps the wall-clock comparison as the *classifier* (given
  `now_ms`, is this turn past budget?) — the timerfd only controls *when
  now_ms is sampled*, so the logic stays pure + unit-testable. No behavior
  change to what a given `(events, now_ms)` produces; only *when* the broker
  re-evaluates.
- `scanRetries` folds into the same deadline source so retries also stop
  riding viewer polling.

This is deliberately staged: Part A lands the fold (real value, low risk);
Part B lands the timerfd once acked.
