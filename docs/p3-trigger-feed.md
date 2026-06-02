# P3 trigger-feed — context-management signals (kvothe lane)

Author: kvothe · 2026-06-01 · For: auri's interim hub context-mgmt + elodin's P3 watchdog
Status: BUILT + increment-c DONE — `bus triggers` writes
`$STATE/triggers/<agent>.json` + renders the ACT/WAIT/OK glance; the
derivation is shared (`src/trigger_feed.{h,cpp}`) and the `bus monitor`
1 Hz loop refreshes the trigger files each tick + renders the alarm gutter
(red ▲ ACT / yellow △ WAIT / blank OK) with a legend footer. Contract
SETTLED with auri + elodin. NEXT lane: task-model → critical-path.

Durable anchor (survives /clear — the #10 lesson). Fresh-me builds from this.

## Goal

Expose the signals a context-manager needs to decide WHEN to intervene
(compact / hand off an agent) so degradation near full context never produces
confident-false reports (THE reliability signal — agents degrade ~100% fill).

**I own the PRODUCER side** (derive the signal from agent state). **elodin owns
the P3 ACTUATOR** (what fires when triggered). **auri's interim hub is the
consumer NOW** (manual context-mgmt before P3 automates).

## The non-obvious insight (why this isn't just "ctx-fill %")

ctx-fill alone is the WRONG trigger. Intervening (inject /compact, hand off)
**mid-turn drops the agent's planned tool calls** — the streamed text finishes
but queued tools vanish ([[mid-stream-silent-dropped-turn]]). So a watchdog that
fires purely on "90% full" mid-task CORRUPTS the agent. The feed's real value is
the SAFETY axis: **is the agent at a subtask boundary where intervention is
safe** — not just how urgent. Two axes, both required:

1. **URGENCY — ctx-fill.** Already in `$STATE/status/<agent>.json`
   (`used_percentage`, `context_window_size`; elodin's pending `context_tokens`
   enriches it). P3 prior-art fires at 50-60%, not 95%. This signal EXISTS —
   the feed just surfaces it cleanly. (Coarse `used_percentage` integer is why
   `context_tokens` matters — see [[project_monitor_column_sources]] /
   observability-viewers.md.)
2. **SAFETY — subtask-boundary.** NEW. The high-value producer work. Strongest
   boundary = an agent-authored `bus done` stamp (`$STATE/done/<agent>.jsonl`,
   which I own) = "I finished a unit of work, safe to compact now." Weaker
   fallback = agent IDLE (turn-axis Ready/None via the broker state RPC, not
   WORKING/mid-tool-call) with no pending mail. The `bus done` connection ties
   the P5 work to P3: the completion verb doubles as the boundary marker.

## Contract — SETTLED (auri + elodin)

All three surfaces, one shared derivation:
- **FILE** `$STATE/triggers/<agent>.json` (elodin's P3 actuator reads;
  decoupled, no live round-trip). Schema (elodin's, exact):
  `{agent, urgency:{ctx_fill_pct, source}, safety:{boundary, done_stamp_ms?,
  idle_ms?}, computed_at_ms}`. `computed_at_ms` ⇒ actuator treats a stale
  file as no-signal (writer is a viewer; viewers die). `boundary` ∈
  `done|idle|none`; **P3 fires ONLY when boundary != none** (never mid-turn).
  Atomic write (tmp+rename) — concurrent reader. NEVER `$STATE/status`
  (elodin's write-owned; clobber).
- **`bus triggers` CLI** (auri's hub glance + the writer): derives → writes
  all trigger files → renders `AGENT | FILL | BOUNDARY | REC`.
  REC = ACT (urgent + boundary!=none) / WAIT (urgent + mid-turn) / OK (not
  urgent). urgent = ctx_fill ≥ 60% (early, P3-style).
- **(c) monitor alarm-zone** — TODO: color rows red=ACT / yellow=WAIT /
  green=OK; + the monitor 1Hz loop calls the shared derivation+write so the
  files stay fresh without a manual poll.

WRITER reliability (settled): v1 = staleness-guarded viewer-write (the
`bus triggers` one-shot + the monitor loop). Failure is SAFE by
construction — writer dies → files stale → `computed_at_ms` guard → P3
reads no-signal → doesn't fire. A missed trigger is benign; a spurious
mid-turn fire is NOT. v2 (move the write into elodin's broker scan) DEFERRED
until staleness-induced missed triggers are actually observed under load —
don't couple the broker to my derivation before it's proven necessary.

## Sources (all already exist; the feed COMBINES them)

- ctx-fill: `$STATE/status/<agent>.json` (read-only consume).
- turn-axis (idle vs working): broker `state` RPC snapshot (the monitor already
  pulls it; see `src/sub/sub_monitor.cpp`). Derivation is read-only.
- boundary: `$STATE/done/<agent>.jsonl` (last completion ts) — I own it.
- DO NOT add a producer to `$STATE/status` (elodin's file — clobber).

## Open / next

1. ~~settle contract~~ — DONE (file + `bus triggers`, schema above).
2. ~~build v1~~ — DONE: `src/sub/sub_triggers.cpp` (`bus triggers`). Verified
   live: kilvin 74%+idle → ACT; mid-turn agents → boundary none; written
   file matches schema.
3. ~~(c) monitor alarm-zone~~ — DONE. Shared derivation lifted to
   `src/trigger_feed.{h,cpp}`; monitor's 1 Hz loop derives + writes every
   tick (continuous freshness, no manual poll) and renders a left alarm
   gutter (▲/△/blank) driven by `recOf`, legend footer only when alarms
   are live. Chose a gutter over full-row recolor so the per-cell signals
   (state glyph, ctx tier, agent color) survive.
4. ~~checkpoint (elodin P2 emit)~~ — **LIVE-VERIFIED** (broker restarted
   on the new binary). `$STATE/status/<agent>.json` now emits top-level
   `model` + `context_tokens` + nested `context_window.{used_percentage,
   context_window_size}`. Confirmed end-to-end:
   - **context_tokens switch** — `bus triggers` writes
     `urgency.source = "context_tokens"` with `ctx_fill_pct` derived from
     raw `context_tokens/context_window_size` (e.g. 53213/200000 → 27,
     finer than the rounded `used_percentage`). Backward-compat fallback
     to `used_percentage` still holds for a pre-P2 emit.
   - **MODEL column** — status `"model":"claude-opus-4-8"` → monitor's
     `contextStatsFor` extracts top-level `model` → `formatModel` →
     `opus-4-8`.
   - **STALENESS observed (benign, by design):** trigger files only stay
     fresh while a writer runs (the `bus monitor` 1 Hz loop). With no
     monitor up, files go stale; the `computed_at_ms` guard makes P3 read
     no-signal → won't fire → SAFE. Implication for the actuator: fresh P3
     signals require a running writer. ([[long_running_viewers_die]])

## LANE EXPANDED — observability pillar (sulin-blessed 2026-06-01)

auri's strategy: `docs/observability-pillar.md` (the what/why anchor; coders
own the how). I own: SPAN SCHEMA (consumer-driven) + CRITICAL-PATH view +
TASK-TRACKING model. elodin instruments the broker to my schema. Same
producer/consumer split as the status-emit.

**My build sequence** (auri's, my call): (c) trigger-feed alarm-zone +
monitor-loop write → TASK-MODEL → CRITICAL-PATH view → SPAN SCHEMA.

**My refinement (proposed to auri):** peel a MINIMAL CORE span schema EARLY
— `{trace_id, span_id, parent_id, stage, t_start, t_end, outcome,
token_spend}` are VIEW-INVARIANT broker-stage primitives. Hand elodin that
core now so he instruments the broker (RPC→cmd-queue→processing→delivery→
dispatch→ACK) in PARALLEL, not serialized behind my view. View-specific
dimensions iterate on top.

**TASK-MODEL = convergence, not greenfield:** `{id, owner, state, deps,
done-claim}` unifies my P5 `bus done` stamps (`$STATE/done`) + the broker
work-queue + agent-state from the trigger-feed (`$STATE/triggers`). Don't
invent a 4th store.

**CRITICAL-PATH view:** trace a task through the broker's instrumented
stages → slowest dependent chain = critical path → which lane/stage gates
throughput. The headline view. (CODE→UTILIZATION/dead-path is the
fast-follow off the same spans.)
