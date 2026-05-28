# claude-bus ideation — elodin

Author: elodin · For: auri (synthesis with bast + kvothe) · Status: brainstorm, not a plan.

Five ideas, ranked by the impact I think they'd have on the fleet's overall health. Effort estimates are rough: XS = under an hour, S = ~half a day, M = a day or two, L = a week+.

## 1. Broker health observability (S) — *highest impact for me*

The single most painful debug in recent sessions was the broker delivery-loop wedge: alive, RPC-responsive, but its `tick()` callback wasn't firing. I had no way to see that without adding heartbeat instrumentation, observing, then yanking the heartbeat. Every future wedge-class bug pays the same setup cost.

What I'd add: a `broker_stats` RPC + a small monitor column. Stats include `tick_count`, `last_tick_elapsed_ms`, `scan_events_bytes_consumed`, `retries_fired_total`, `escalations_total`, `auto_clears_total`. Read via `bus broker stats` (existing) and surfaced as a TICK column in `bus monitor` (one number, dim when fresh, red when stale > 30 s).

Why this matters: the wedge bug pattern is "broker stays alive, internal loop stalls." Heartbeats catch this immediately. The same instrumentation also makes "is auto-clear actually firing?" / "are records actually retrying?" answerable without grep-fu.

Out of scope here: full structured logs. Just enough to detect the alive-but-stalled state from a glance.

## 2. Ship the hybrid delivery from `docs/delivery-alternatives.md` (S)

The proposal in #47 lays out a hybrid: today's push as fast path + per-agent `/loop 30s bus msg fetch inbox-<self>` as fallback + a "fetch skips in-flight" rule to prevent double-delivery. ~20–30 LOC plus a role-prompt line per agent.

The win is graceful degradation: today's push failures (mid-stream drops, scrolled panes, `mode=unknown` defers, the wedge class) become latency increases bounded by the poll interval, instead of records sitting indefinitely. No wire-format change, no restart required to gain the fallback.

I'd hold this on the routing-decision side; the design is approved-shape but not approved-to-ship.

## 3. Default TTLs per topic kind (XS) — quick reliability win

`ttl_ms` is in the wire format but always 0 (never expires). The escalation spam I saw in `inbox-ops` post-restart (recursive stale-epoch chains, 100+ records) would have self-pruned if records carried a reasonable TTL.

Proposal: per-kind default TTLs.
- `agent-inbox`, `tui-commands` — 1 hour (mail older than an hour is rarely actionable).
- `audit`, `append-log` — 0 (audit needs to be durable).
- `work-queue`, `pubsub`, `blackboard` — 0 (semantically valid forever).

Implementation: `bus msg mail` / `bus msg slash` / `bus msg broadcast` stamp a default TTL based on the target topic kind. Override with `--ttl`. Broker's existing TTL check at dispatch handles expiry; just need defaults at the producer side. Few-dozen LOC across CLI + broker enqueue.

## 4. Hard-death detection (phase 2 of auri #10) (M)

Phase 1 (just shipped) handles soft death — `SessionEnd` cleanly emitted. Phase 2 catches the case where the pane vanishes without `SessionEnd`: zellij kill, hard crash, host went away. Per-agent "first-seen-pane-empty-at" timestamp in the broker; after N minutes empty, escalate the head record(s) via `escalate()` and advance the cursor.

Why M not S: needs careful interaction with the auto-clear cooldown and the agent-respawn path. The N-minute threshold needs tuning. Risk of false-positive escalation on transient zellij hiccups.

Worth it because the wedge class today is *records pile up forever for a hard-dead agent* — no retry, no escalation. That's the worst kind of failure: silent and unbounded.

## 5. Rebuild integration tests (L)

`tests/bus-itest.sh` predates many of this session's changes. Sulin flagged it as "busted" early in the session and I've been verifying changes by hand ever since. Every change risks a regression I can't catch automatically.

What rebuilding looks like:
- Isolated `CLAUDE_BUS_STATE` per test (already the pattern), short-lived broker per case.
- One file per concern: `tests/broker-singleton.sh`, `tests/dispatch-ack.sh`, `tests/epoch-quarantine.sh`, `tests/auto-clear.sh`, etc.
- A common shell harness for "boot broker, run scenario, assert state, kill broker."
- CI in any form (a GitHub Action triggered on push; nix flake check; whatever lands lightest).

L because doing this properly means triaging which existing test cases still match the design vs which were written against the older mailbox/watcher shape that's now retired. Probably a week of work to get to "every PR auto-runs and fails on real regressions." But the leverage is permanent — every future bug-class becomes a one-line addition to the suite.

## Stack-rank if forced

If auri asks "which one first":
1. **#1 broker health observability** — small, high impact, complements the recent reliability fixes. The next wedge happens regardless of what else we do; this makes it 10× cheaper to debug.
2. **#3 default TTLs** — quickest win on the list.
3. **#2 hybrid delivery** — already designed; pull-fallback meaningfully reduces user-visible failure rate.
4. **#4 hard-death phase 2** — closes the most expensive remaining failure class but needs care.
5. **#5 tests** — highest long-term leverage, biggest investment. Worth scheduling as its own block rather than interleaved with feature work.

## Not on this list

- **Multi-broker federation.** Out of current scope. Could matter once we run distinct fleets per project.
- **Wire format v5 with a real epoch field.** The correlation-field repurpose works; cleanup is cosmetic.
- **Cost / token dashboards.** Statusline JSON has `cost.total_cost_usd`, kvothe's territory; mentioned in `docs/context-budget.md` and `docs/observability-research.md`.
- **`/clear` automation tuning.** Already shipped; tune over time as we see false-positive / negative rates.
