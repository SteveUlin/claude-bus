# What to work on next — ranked (2026-06-08)

Output of the `improvement-survey` workflow (6 parallel audits — code-quality,
reliability, feature-gaps, observability/DX, prior-art web research, Claude-Code
leverage — → opus rank → adversarial rerank → finalize). Ranked by
**value-per-effort**, deduped against the tracked backlogs, cursor/agent-type
work hard-excluded. Each item carries honest effort/risk and `file:line`
evidence.

**Thesis:** wire the itests into ctest *first* (the safety net every delivery-loop
edit below needs), then walk the verified off-TTY spine — continuity grace →
readiness sentinel Slice 1 → harden the now-default drain path — folding in the
cheap monitor-truth fixes. No shiny platform bets; every item traces to a
verified bug or sulin's stated focus.

## Ranked

| # | Item | Cat | Effort/Risk | Status |
|---|------|-----|-------------|--------|
| 1 | **Wire 18 itests into ctest** (`itest` label) | code-quality | low / low | new |
| 2 | **Continuity grace in `maybeEscalateStuck`** | reliability | low / low | new |
| 3 | **Readiness sentinel — Slice 1 only** (`isAgentIdle`) | feature | low / med | tracked (agent-contract), unbuilt |
| 4 | **Age-release stale drain in-flight** | reliability | med / med | new latent bug |
| 5 | Activate RecoveryActor Phase B (+ AutoClearActor extract) | reliability | med / med | tracked (P2 Phase A landed) |
| 6 | `tasks open` writes `$STATE/title` (TASK-column lie) | observability | low / low | new |
| 7 | `token_watcher` float-guard + blackboard `dump()`→`tail(1)` | reliability | low / low | new |
| 8 | Audit events in `bus log` + strand signal in monitor | observability | low / low | new |
| 9 | Harden `isOrphanBroker` vs PID reuse | reliability | low / low | new |
| 10 | `tui-commands` retry: ack-deadline-then-escalate | reliability | med / med | new / known-deferred |
| 11 | `bus config-check` + monitor last-refreshed stamp | dx | low / low | new |

## The spine (1–4) — detail

**1 — Wire the itests into ctest.** `CMakeLists.txt:198` registers only
`add_test(NAME unit COMMAND bus_tests)`. The 18 `tests/*-itest.sh` are hermetic
(isolated `$STATE`, fake-zellij on PATH, self-cleaning) and are the **sole**
correctness gate for `delivery::Loop` — the ~1500-line at-least-once / ack /
retry / escalate / drain engine with **zero** unit coverage — yet ctest never
runs them. Add per-script `add_test(... LABELS itest)`; `ctest -L unit` stays
fast, `ctest -L itest` runs the real gate. *This is the precondition that makes
every edit below safe* — and the harness that lets us turn the claimed bugs in
#4/#10 into reproducing tests before fixing them.

**2 — Continuity grace in `maybeEscalateStuck` (`delivery.cpp:898`).** It uses
raw `nowMs()` against `turn_start_ms` / `open_tool_since_ms` with no continuity
floor, though `updateContinuity()` already ran first in the tick and
`recovery_actor.cpp:78` already has the grace for *actions*. A lid-close/resume
instantly inflates every open turn's age → fleet-wide false turn-stuck/tool-
wedged **audit alarms**, training the operator to ignore alarms. ~10-line mirror
of an already-computed signal. Serves monitor-must-not-lie.

**3 — Readiness sentinel, Slice 1 ONLY (`isAgentIdle`, `delivery.cpp:282`).**
The off-TTY focus. Add `ready-write.sh` (Stop + SessionStart) → atomic
`$STATE/ready/<name>.json`; add `src/ready.{h,cpp}`; `isAgentIdle` reads the
sentinel instead of forking `paneStateCached`, behind `CLAUDE_BUS_PANE_FALLBACK`
(load-bearing — a missing/stale sentinel must fall back, not wedge delivery).
The compact carve-out is sentinel-schema design (on `source=compact`, write a
compacting-marker the broker reads as not-idle), **not** a one-line matcher.
Leave the doorbell fork (`:1406`) + `maybeWakeIdleOffTty` as a **separate** Slice
2, gated on agent-contract.md's 3-check soak. Un-conflating these is the point.

**4 — Age-release stale drain in-flight.** A genuinely-new latent bug on the
**fleet-default** path: `noteDrainDelivery` sets `next_retry_at=0`
(`delivery.cpp:365`) so `scanRetries` skips it and `maybeEscalateStuck` ignores
it; the next drain hits the `already_inflight` guard (`broker.cpp:909-910`) and
re-skips. A drained off-TTY record whose `bus-ack` never lands is **frozen until
SessionEnd** — the strand watchdog *alarms* (mail stays unread) but the
doorbell's corrective drain is a no-op for that record. The push path escalates
after `kMaxAttempts`; the drain path has no terminal escalation. Fix: an
ack-deadline in `noteDrainDelivery` that on expiry clears the in-flight entry so
the next drain re-delivers. **Verify with a reproducing itest first** (this is a
traced claim, not yet a proven failure — #1 gives the harness).

## Parked — with reasons (do NOT silently re-discover these)

- **Cross-cutting RULE — probe unverified Claude-Code features before designing
  on them.** The Claude-Code research cited hooks (`FileChanged`, `watchPaths`,
  `StopFailure`, `PostCompact`, `claude agents --json`) from a docs URL that
  could not be verified (future-dated). Several would *moot* the Slice-2 doorbell
  if real. Spend 30 min wiring one in a throwaway settings and confirm it fires
  with the documented payload **before** any roadmap leans on it. Verify-then-design.
- **Kernel-unit-extraction of `scanEvents`/`scanRetries` behind seams** — double-
  counts `broker-seam-redesign.md` (Router.onAck, Log). Already tracked.
- **Consolidate the 7 per-tick `readAgents` scans into one fold** — the intra-tick
  snapshot-skew angle is the only new contribution; the consolidation itself lands
  *inside* the Readers seam (the 7 callers pass different filter/continuity args,
  so a naive fold would regress #2's grace). Park the skew note for that seam.
- **Zellij native plugin (Rust/WASM) to replace the shell pane-read** — real
  long-term fit for stop-pane-reading, highest-effort shiny bet; distracts from
  the verified near-term off-TTY work. Design note only.
- **Deterministic Simulation Testing / linearizability property tests** — real
  best-practice, high-effort (clock+socket injection through `rpc.cpp`/
  `delivery.cpp`). #1 captures most of the regression-net value far cheaper.
- **OTel traceparent through the Envelope / enhanced-telemetry token traces** —
  genuine value, med-high effort, adds an `opentelemetry-cpp` dep, and the token-
  source angle is gated behind the unverified-feature probe. Park.
- **DLQ + replay, backoff-jitter, inotify CDC wakeup, transactional-outbox docs**
  — sound prior-art but premature (DLQ before #4's drain-release), a tuning change
  with no observed pathology (jitter — limit-cadence is the right gauge), or
  latency optimization the correctness bugs outrank.
- **cursor-cli / heterogeneous agent-type integration** — hard-excluded per
  sulin's deferral. See `docs/agent-contract.md`.
