# Reporting-truth: making post-reboot fleet state honest

**Owner:** kvothe (monitor-truth domain) · broker-side inputs: elodin
**Status:** DESIGN — awaiting auri/sulin ratify before build
**Task:** #1780679517444-kvothe-a3d2

## The lie

The displayed lifecycle state and the broker's own escalation logic
disagree about what "stuck" means, and the disagreement is the bug.

- **`maybeEscalateStuck`** (`delivery.cpp:884`) is *structure-aware*. It
  alarms only when the D8 fold accumulator shows an unmet obligation past
  budget: `open_tool` overdue (a PreToolUse with no PostToolUse) or
  `turn_start_ms` overdue (a turn that opened and never closed).
- **`computeAxes`** (`agent_status.cpp:298`) computes the *displayed*
  `TurnAxis::Stuck` from **raw last-event age** alone:
  `age_s > 5min → Stuck`. It ignores the fold structure entirely.

So an agent whose last event is `PostToolUse:Bash` ten minutes ago, with
no tool in flight, is *not* alarmed by the broker — but the monitor paints
it red STUCK. The header already names the fix: the fold accumulator is
"Part A … computeAxes' axis outputs do NOT yet depend on these (the
wall-clock decouple is **Part B**)" (`agent_status.h:44`). **Part B is
this workstream.**

Three honest distinctions are collapsed today:

| Reality | Today's label | Honest label |
|---|---|---|
| Tool in flight, overdue — genuinely wedged | STUCK 🚧 (red) | STUCK 🚧 (red) ✓ |
| Quiet a while, nothing in flight — parked / lost-Stop / long-think | **STUCK 🚧 (red)** ✗ | **QUIET 🌙 (dim)** |
| Last real event predates a reboot/suspend — age inflated by the sleep gap | **STUCK 🚧 (red)** ✗ | reconciled — age clamped to continuity |
| Mail queued, agent can't take it — needs a human nudge | **HAS_MAIL 🔔 (benign cyan)** ✗ | **NEEDS_NUDGE ⚠ (yellow)** |

## (b) Stale-turn reclassification — the core fix

Make the displayed turn axis agree with the escalation logic: key STUCK
off *structure*, not raw age. Replace the single `age_s > 5min → Stuck`
rule with a two-way split that mirrors `maybeEscalateStuck`:

```
when process == Alive and the turn is stale (past the 5-min budget):
  open_tool != ""  and  now - open_tool_since_ms > tool_budget
      → TurnAxis::Stuck        (wedged: a tool call opened and never returned)
  open_tool == ""  and  now - turn_start_ms      > stuck_budget
      → TurnAxis::Quiet        (NEW: no obligation in flight, just silent)
  otherwise (recent)
      → Working / Orchestrating, as today
```

The disambiguator is `open_tool`: an in-flight, overdue tool call is a
real wedge (red). A turn that's merely silent — Stop event lost, claude
thinking a long time between tools, or the mid-stream dropped-turn — has
no in-flight obligation. That is **QUIET**, not STUCK.

**New `TurnAxis::Quiet` / `State::Quiet`.**
- Glyph `🌙`, color **dim** (never red). "No events for a while; nothing
  is in flight. Probably parked at the prompt — glance if you like, but
  nothing is hung."
- Distinct from `IDLE` (💤 green): IDLE means the turn *cleanly closed*
  (Stop / idle_prompt) — certain-ready. QUIET means the turn never closed
  but nothing is wedged — probably-ready, uncertain.
- Distinct from `STUCK` (🚧 red): STUCK is now reserved for a
  structure-confirmed wedge.

This is what stops the monitor crying STUCK on healthy idle panes.
Detection is **not** weakened: `maybeEscalateStuck` is unchanged and
still audit-alarms turn-stuck/tool-wedged; the P3 trigger feed + alarm
gutter still drive ACT/WAIT. We are making the *label* honest, not
dropping the watchdog. The dropped-turn failure mode (silent stall, empty
prompt) correctly reads QUIET and, if it has mail, escalates via (c).

Budgets stay the env-tunable `CLAUDE_BUS_STUCK_BUDGET_MS` /
`CLAUDE_BUS_TOOL_BUDGET_MS` the broker already reads, so the display and
the escalation share one knob.

## (a) Post-reboot / suspend reconciliation

After a reboot or lid-close, the wall clock jumps. Every agent's
last-event `age_ms` inflates by the off/asleep gap, so the whole fleet
flips to STUCK on resume even though nothing is wrong (the
suspend/resume wall-clock failure mode). The fix: an event that predates
the last *continuity boundary* must not have its pre-boundary age counted
as staleness.

Define a `continuity_since_ms` — the earliest wall-clock instant we can
trust event ages against — and clamp the staleness clock:

```
effective_age = now - max(event_ts_ms, continuity_since_ms)
```

`continuity_since_ms = max(system_boot_ms, broker_continuity_ms)`:

- **`system_boot_ms`** — from `/proc/stat`'s `btime` (epoch seconds of
  last boot; confirmed present). Handles power-cycles entirely
  model-side; **I own this**, read it in `agent_status.cpp`.
- **`broker_continuity_ms`** — handles *suspend* (btime is unchanged
  across a lid-close). Only the continuously-running broker can observe a
  monotonic-vs-wallclock jump. **elodin's input** (see Coordination).

Right after a discontinuity, every agent reads ~0 effective age → not
stuck. As real post-reboot events land (relaunch → SessionStart(resume) →
Alive/Ready → IDLE), normal flow resumes. This also means a genuinely
pre-reboot mid-tool agent isn't falsely accused of a 10-minute wedge that
was really 9 minutes of sleep.

`computeAxes` gains an optional `continuity_since_ms` parameter (default
0 = today's behavior, so every existing caller and test is unaffected
until the broker passes it).

## (c) Mail-can't-inject → NEEDS_NUDGE

Today `mail.Pending → HAS_MAIL` conflates "queued, will deliver" with
"queued, agent can't take it." The disambiguator already exists:
`wakeReadyForMail(axes, pane)` *is* "can the broker push to this agent
right now?"

```
mail.Pending and agent will drain on its own
    (wake-ready, or actively Working/Orchestrating — turn ends → delivered)
      → HAS_MAIL 🔔   (benign; broker has it)
mail.Pending and agent will NOT drain on its own
    (Quiet / Stuck / locked pane / boot-stuck)
      → NEEDS_NUDGE ⚠  (NEW: queued but blocked — a human nudge unsticks it)
```

A Working agent with fresh mail stays HAS_MAIL — the broker correctly
defers mid-turn (mid-stream dropped-turn hazard) and delivers when the
turn ends; that's healthy queuing, not a stuck condition. NEEDS_NUDGE
fires only when no self-recovery path exists.

**New `State::NeedsNudge`.** Glyph `⚠`, color **yellow** (attention, not
alarm-red — matches the NeedsInput intervention idiom). It rides the mail
axis, so it composes with the turn axis rather than replacing it: an
agent is QUIET *and* NEEDS_NUDGE; the single-State shim picks NEEDS_NUDGE
(the actionable one) for the cell.

**v1 is purely model-side** — derived from `pending + not-self-draining`,
no new broker input required. **Enhancement (coordinate w/ elodin):** if
the broker exposes a per-agent delivery-retry count or an
inbox-human-escalated flag, NEEDS_NUDGE gets a stronger, earlier signal
(the broker *knows* it tried and failed) instead of inferring from
readiness. Not required for v1.

## Surfaces touched (all mine)

- `agent_status.{h,cpp}` — `TurnAxis::Quiet`, `State::Quiet`,
  `State::NeedsNudge`; structure-aware turn split in `computeAxes`;
  `continuity_since_ms` param + `systemBootMs()` helper; glyph/color/name
  for the two new states; `computeStateFromLabel` inverse in the monitor.
- `broker.cpp` state RPC (the seam) — pass `info`'s fold fields (already
  in scope) + `continuity_since_ms` into `computeAxes`; put the new state
  labels on the wire (no schema change — `state` is already a string).
- `sub_monitor.cpp` / `sub_agent_bar.cpp` — render the new glyphs/colors;
  `computeStateFromLabel` learns QUIET / NEEDS_NUDGE.
- `trigger_feed.cpp` — QUIET is a *safe* boundary (between-turns, nothing
  in flight) → falls through to `Boundary::Idle`, already correct;
  NEEDS_NUDGE likewise. Verify the label checks don't misfile them.

## Coordination with elodin (broker-side inputs)

1. **`broker_continuity_ms`** — the broker is the only process that can
   detect a *suspend* jump (btime catches reboot, not suspend). Ask: on
   each delivery tick, if `nowMs()` jumped forward more than a threshold
   (e.g. > 2× the expected tick gap), record the resume instant; expose
   it on the `state` RPC (or a `$STATE` file) as `continuity_since_ms`. I
   take `max(systemBootMs(), that)`. The broker may already have a
   process-start / boot-epoch timestamp that serves as a first cut.
2. **(optional) delivery-retry / inbox-human-escalated per agent** —
   strengthens NEEDS_NUDGE from inferred to broker-confirmed. Nice to
   have, not gating.

Everything else is model-side and ships without broker changes; the
`continuity_since_ms` param defaults to 0 so I can land (b) and (c) first
and wire (a)'s suspend half when elodin's signal exists.

## Build order

1. **(b) DONE** (main `3d894c7b`) — structure-aware turn split + `QUIET`.
2. **(c) DONE** (main `3d894c7b`) — `NEEDS_NUDGE` off `wakeReadyForMail`.
3. **(a) model-side primitive DONE** — `systemBootMs()` (reboot half) +
   `computeAxes`/`computeState` `continuity_since_ms` param (default 0 =
   no-op) that clamps staleness to `max(event_ts, continuity_since_ms)`.
   **Broker wiring PENDING (elodin):** the call sites (broker `state` RPC +
   delivery/recovery) pass `max(systemBootMs(), continuity_since_ms)`, where
   `continuity_since_ms` is the loop-plane Δwall−Δmono resume instant. Until
   then the clamp is dormant; the reboot half goes live the moment a caller
   passes `systemBootMs()`.

The consumer contract (stable, build against this):

```
// agent_status.h
auto computeAxes(... , const PaneState* pane = nullptr,
                 std::int64_t continuity_since_ms = 0) -> AgentAxes;
auto systemBootMs() -> std::int64_t;   // /proc/stat btime → ms, 0 if absent
```

Each step is independently shippable and verifiable (unit tests over
`computeAxes` with synthetic `AgentInfo` fixtures).
