# orchestrating-state — a receptivity axis for background work (kvothe lane)

Author: kvothe · 2026-06-02 · For: sulin's "the IDLE/WORKING binary can't
express background orchestration" (surfaced by Chronicler/ultracode).
Status: **DESIGN — surface before implementing.** State-model + monitor are
my lane; the broker defer-gate change is elodin's. See [[project-orchestrating-state]],
[[project_agent_states]], builds on the orthogonal-axes model in
`agent_status.h`.

Durable anchor (survives /clear).

## The problem

The lifecycle model collapses to IDLE vs WORKING. A Workflow run, a `/loop`,
a long coordination turn dispatching subagents — all read **WORKING**, so
`bus state` calls an orchestrating agent "working" when it is really *waiting
on background work and receptive between dispatches.* sulin's framing: **the
axis that matters is RECEPTIVITY, not busyness.**

It is not cosmetic. It changes the **broker defer gate**: the broker must
DEFER mail to a true mid-turn WORKING agent (injecting there risks the
mid-stream dropped-turn hazard, [[project_mid_stream_silent_dropped_turn]]),
but it can safely deliver to an ORCHESTRATING agent, which drains at its next
sub-boundary.

## A sharper look — what's actually load-bearing

Background tools **return immediately**: `Workflow` returns a task id and runs
agents in the background; `run_in_background` Bash/Task the same; `/loop`
sleeps between turns. So the orchestrator's main loop already **oscillates
Ready↔Working** and the broker can already deliver in the Ready windows. Two
things are genuinely missing, and they're what this state buys:

1. **A truthful label.** sulin sees WORKING and reads "don't interrupt"; the
   agent is actually receptive. The monitor must *show* the orchestration
   posture distinctly.
2. **Defer-gate intent during the Working windows.** Inside a long
   coordinating turn (many dispatches, no Stop for a while) the agent reads
   WORKING and the broker DEFERS — even though this agent drains mail at every
   sub-boundary and injecting at a boundary is safe. The state tells the gate
   "this WORKING is the receptive kind."

## Where it fits — a new TurnAxis value, not a new concept

Reuse the orthogonal-axes model already in `agent_status.h`. Add **one**
`TurnAxis` value:

```
enum class TurnAxis {
  None, Ready, Working, Stuck, NeedsInput, Compacting,
  Orchestrating,   // work live but RECEPTIVE — drains at sub-boundaries
};
```

`State` (the compat shim) gains a matching `Orchestrating`. Process / Mail /
Tui axes are untouched. This keeps the design inside the existing derivation;
no new store, no parallel state machine.

## The payoff — the broker defer gate (elodin's lane)

The one behavioral change: `wakeReadyForMail` (the doorbell/defer readiness
predicate, `agent_status.h:195`) returns **true** for `Orchestrating` as it
does for `Ready` — so the broker delivers instead of deferring. A true
`Working` still defers. That is the whole point of the state; everything else
(label, glyph) is presentation. **I design + expose the axis value + its
signal; elodin owns flipping the gate to honor it.**

## The detection question (the crux) — RECOMMENDATION: hybrid w/ auto-decay

Receptivity-while-working is a *posture*, and a single event rarely shows it
(background tools return immediately, so "I dispatched and I'm waiting" looks
identical to "I finished"). Three readings:

- **A · events.jsonl markers (D8 fold).** Treat `open_tool ∈ {Workflow,
  Task(background)}` as Orchestrating. *Limit:* a SYNCHRONOUS `Task` blocks
  the main loop (NOT receptive) — must distinguish background; and completion
  tracking is fiddly. Cheap, zero agent burden, but noisy.
- **B · self-declared sentinel + TTL.** The agent (or the orchestration skill
  / `/loop`) writes a `$STATE/orchestrating/<agent>` marker on entry, clears
  it on exit — mirrors `[bus-attach]` presence + the trigger-feed staleness
  contract. *Most truthful about intent* (only the agent knows its posture),
  but adds burden and staleness risk (forgets to clear → lies forever).
- **C · HYBRID with auto-decay (recommended).** Events.jsonl Workflow /
  background-spawn markers **auto-set a short-TTL** orchestrating flag (zero
  burden, self-clearing) — an agent that uses Workflow gets the state for
  free. PLUS an optional explicit sentinel for postures events can't see (a
  long coordination turn, a `/loop`). **Both carry a TTL / heartbeat**, so a
  crashed orchestrator decays to its real state (IDLE/STUCK) rather than
  lying. The TTL is what makes self-declaration safe — it's the same move the
  trigger feed uses (`kOwnerLiveStaleMs`): a declaration is a *lease*, not a
  latch.

Recommendation **C**: auto-trigger from the events the harness already emits,
backstopped by a lease TTL, with an explicit opt-in for the event-invisible
cases. Truthful, low-burden, self-healing.

### Open detection sub-questions (for sulin / auri)
- Does `events.jsonl` already carry a distinguishable **background**-Task /
  Workflow spawn marker, or does a hook need to stamp one? (Drives whether A's
  auto-trigger is free or needs a hook line.)
- TTL length: short enough to decay a dead orchestrator fast, long enough to
  survive a quiet stretch between dispatches. Trigger-feed uses 30 s; a
  ~60–90 s lease with re-stamp on each orchestration event feels right.

## Monitor render (my lane)

A distinct glyph + color so the posture reads at a glance and is NOT confused
with WORKING — e.g. 🎛️ / cyan family (receptive), vs WORKING's 🔨. The CTX /
EFFORT / MAIL columns are unchanged. `bus state` prints `ORCHESTRATING`.

## Scope / routing

- **kvothe (me):** the `TurnAxis::Orchestrating` value, its derivation in
  `computeAxes` (the chosen signal), `stateName`/`stateGlyph`/`stateColor`,
  the monitor + `bus state` render. Design-doc-first (this doc).
- **elodin:** the defer-gate change — `wakeReadyForMail` honoring
  Orchestrating, and any delivery-loop consequence. Broker internals.
- **sulin / auri:** ratify the detection signal (A/B/C) + the TTL, and the
  glyph/label.

Ties the #16 observability blind-spot (monitor blind to Workflow subagents,
[[project_p3_context_watchdog]]).

## Status / next

1. ~~ground the design in the axes model + defer gate~~ — DONE (this doc).
2. ~~surface to auri/sulin: detection signal + TTL + glyph~~ — RATIFIED
   (auri): C (hybrid auto-decay), 90 s lease, glyph my pick.
3. ~~implement the axis + signal + render (my lane)~~ — BUILT + LANDED
   (main 8d94c32d). `TurnAxis::Orchestrating` + `State::Orchestrating`;
   detection = `last_orchestration_ms` folded from `Workflow` PreToolUse,
   TTL-decayed at 90 s in `computeAxes` (overrides ONLY mid-turn Working,
   never Ready/Stuck/NeedsInput); render = 🪐 bright-green across monitor /
   agent-bar / deck / `bus state`; trigger-feed treats it `boundary=none`
   (context-mgmt-unsafe, like Working — net-zero since it was Working
   before). 7 unit tests (fold stamp/decay/reset; computeAxes lease /
   decay / ready-not-overridden / stuck-outranks). **Behavior-neutral:**
   `wakeReadyForMail` untouched, so delivery is unchanged — only the label
   becomes truthful.
4. **ELODIN (the activating change):** flip the defer gate —
   `wakeReadyForMail` (agent_status.cpp) returns true for
   `TurnAxis::Orchestrating` as it does for `Ready`. That converts the
   workflow Working-windows from deferred to deliverable (the payoff).
   Until then the state renders honestly but defers like Working — no
   regression either way. Pair on the predicate when he's ready.
5. **v2 (later):** explicit sentinel for event-invisible postures (`/loop`,
   background Bash/Task) — needs tool-input capture or a self-declared lease
   file; the auto-decay TTL contract already supports it.
