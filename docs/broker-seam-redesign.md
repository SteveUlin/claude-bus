# Broker seam redesign — shatter `delivery::Loop` into Log / Router / Transport / Readers / Policy

Status: **design / proposal** (elodin, 2026-06-03). Source: chronicler's run-2
redesign artifact (`/tmp/claude-bus-redesign/data.json`), routed by auri. This
doc gates the refactor: **auri acks the doc → code lands incrementally behind
it.** It does NOT commit the whole 45→7; the contested boxes (§6) are
design-gated separately on sulin's scope call. Owner: elodin.

## 1. Thesis — keep the kernel, shatter the loop

The bet is *"a durable out-of-context broker is a cheaper, more reliable
system-of-record than in-context coordination."* Its irreducible kernel is a
**triad**:

- an append-only, **byte-offset-addressed per-topic Log**,
- a per-(topic,consumer) **cursor that advances ONLY on an ack**,
- a **boot-epoch stamp** on every record.

That triad *is* at-least-once + dedup + restart-safety simultaneously: an
un-acked record sits at the head and is re-evaluated (retry is free, not
machinery); `lastId` at the head is the dedup floor; the epoch fence stops a
surviving disk log from re-delivering yesterday's mail after a reboot. One
single-threaded actor decides when the head may move.

That ~150 lines of irreducible delivery is today wrapped in a **74 KB
`delivery::Loop`** that *also* scans tokens, auto-clears idle agents, runs the
(inert) recovery engine, and trims logs — nine state machines the tick calls in
sequence (`delivery.cpp:874-891`). The redesign keeps the kernel and splits the
Loop along the seams three independent analyses all found.

## 2. The seams

| Component | Owns | Interface (sketch) | dependsOn |
|---|---|---|---|
| **Log** | durable bytes + cursor + epoch; **retention/trim math** | append; peek/dump; cursor read/advance; trimHead | — |
| **Router** | in-flight + gate stack + **ack-deadline** (retry == the deadline) | `tick()`→≤1 gated record/inbox via Transport; `onAck(msgId)`→cursor advance + in-flight clear; `drain(agent)`; `drop(msgId)`; `nextDeadlineMs()` | Log, Transport, Readers |
| **Transport** | last-mile actuator | `deliver(record)`→{Delivered\|Deferred} | — |
| **Readers** | pure derivations only — no authority, no caches (pane caches stay in the loop plane; `PaneState` is injected) | snapshot for Router gates + viewers | Log |
| **Policy** | autonomous-actor decisions + own cooldowns | reads a Readers snapshot, enqueues actions **through Router** | Router, Readers |
| **Daemon** | OS resources + wiring loop; **no message state** | `run()`; `serve(req)`; `arm(nextDeadline)` | Router, Readers |

- **Router advances a cursor ONLY on ack and emits to a Transport sink — it
  never types into a pane itself.** That single rule is the whole at-least-once
  invariant made structural.
- **Transport arms:** off-TTY (default) = a no-op marker; the agent's drain
  hook pulls via `Router.drain`. TTY (opt-out set) = `sendToPaneSafe` under the
  per-pane flock + draft-preserve + INSERT-normalize. The doorbell wake is
  `deliver()`'s asleep-recipient branch. `tty_policy.h isOffTty` selects the arm.
- **Readers** consolidates the derivation core (`parseEvent`/`readAgents`/
  `computeAxes`/`computeState`/`foldTurnState`), token-scan (EVICTED from the
  tick where it never belonged), trigger_feed, task_model, `verify`. It owns
  **no mutable authority** — every output reconstructs from `events.jsonl` + an
  **injected `PaneState` value**, so losing a cache costs nothing.
  **Pane acquisition stays OUT (kvothe's value-boundary correction).** The
  pane-dump fetch and its two process-static TTL caches (`paneStateCached`,
  `listPanesJsonCached`) are lock-free ONLY because the single loop thread owns
  them; pulling them into Readers would re-introduce a shared mutable field and
  break the no-authority invariant on day one. So pane acquisition + its caches
  live in the **loop/Daemon plane** (where the single-thread safety already
  holds); Readers consumes `PaneState` as a value parameter, exactly as
  `wakeReadyForMail(…, const PaneState*)` does today. The derivation core
  already never calls `paneState()` internally — every `paneStateCached` call
  site is in broker/delivery/dispatch, never the core — so this boundary
  already exists in the code; drawing it explicitly costs nothing.
- **Policy** is where future coordination patterns plug in WITHOUT touching the
  kernel — *a coordination pattern is a Policy actor*. The recovery signature
  table (`maybeAutoRecover` / P2) is the first Policy actor.

## 3. The litmus — a seam is real iff it's testable without a broker

The redesign's own test of a real cut vs a relabel: **`Router.plan()` must be
unit-testable with a fake Log + fake Transport + a synthetic Readers snapshot —
no socket, no pane, no real time.** Today the gate logic cannot be tested
without standing up the whole 74 KB Loop, because the Loop co-owns nine state
machines fused only because the Loop is the one thing that ticks. "If
`Router.plan()` needs a live socket to set up, the cut was dressed up by a
misleading count." Every seam below is justified by this litmus, not by the
45→7 headline.

## 4. Extraction order + gates (bottom-up; Policy is a DAG leaf, so last)

Dependency order forces the sequence — **Policy `dependsOn` Router+Readers, so
it can't be first.** Bottom-up:

1. **Unimpeachable cut #1 — trim → Log.** Retention math moves into Log so it
   stops shifting byte offsets *mid-iteration* of the dispatch loop.
   Behaviorally neutral. **Lands freely** (post-P2 + doc-ack), deploy-verify.
2. **Unimpeachable cut #2 — split `scanEvents`' parse-vs-ack weld.** A reader
   folds events → an ack signal; **`Router.onAck`** applies cursor-advance +
   in-flight clear. Behaviorally neutral. **Lands freely**, deploy-verify.
3. **Transport / Readers extraction.** **Design-gate BITES here** — the
   contested shapes (§6) wait on sulin's scope call + the design pass. This doc
   *proposes* their shape; code waits for the gate.
4. **Policy = the completed P2 auto-recovery, extracted last** into a Policy
   actor (proof-of-seam for the Policy boundary — extract a finished, tested
   unit, never build the breaker INTO freshly-refactored code).

**Gate summary (auri, precise):** the two unimpeachable cuts land freely after
P2 + doc-ack (behaviorally neutral, deploy-verify each); Transport/Readers code
waits on sulin's scope call; Policy is the extracted completed-P2; **the broker
delivers mail at EVERY commit; the doc gates the refactor, auri's ack gates
code.** Never big-bang.

## 5. The two unimpeachable cuts (first landable steps, detailed)

Both are behaviorally neutral and same-file (`delivery.cpp`) — which is exactly
why **P2-first is also collision-safe**: the breaker lives in the same file the
cuts touch, so finishing P2 before the cuts avoids a self-collision.

- **trim → Log:** today `maybeTrimLogs` runs inside the tick and rewrites topic
  logs, shifting every absolute offset; the dispatch loop then rebases cursors/
  in-flight. Moving the trim+rebase math behind the Log boundary makes "the
  bytes and their addressing" one owner, so no other component observes an
  offset shift it didn't request. Test: the existing retention itest + unit
  `planTrim` cover it; assert dispatch never sees a mid-iteration shift.
- **scanEvents split:** today `scanEvents` both *parses* events.jsonl AND
  *mutates* ack state (advance cursor + clear in-flight) in one weld. Split:
  a pure reader yields `{ackSignal}` from the tail; `Router.onAck(msgId)` is the
  only thing that advances a cursor or clears in-flight. Test: feed the reader a
  synthetic event tail, assert `onAck` advances exactly the acked cursor.

## 6. Contested boxes — proposed shapes + conceded risks

auri: *the doc PROPOSES their shape; code waits for the gate.* The artifact is
honest that these two are relabel-risk:

- **Readers megafold (risk RESOLVED by kvothe, the Readers owner):** the
  honest-risk framing said a ~1/7 fold in one box is the god-object risk
  relocated to the read plane. kvothe verified it **evaporates once pane
  acquisition is drawn out** (§2): the only stateful part (pane.cpp's two
  process-static TTL caches) was never in the box once Readers consumes
  `PaneState` as an injected value. What remains is **four already-separate,
  already-unit-tested pure TUs** (agent_status/derivation, trigger_feed,
  task_model, verify) sharing only the `(events.jsonl + injected snapshot) →
  value` contract — `test_fold`/`test_agent_status`/`test_readiness` exist
  *today*, broker-free, which is the litmus already passed. Nothing to fuse ⟹
  no god-object **by construction**. **Proposed shape:** keep them sibling
  free-function TUs under a `bus::readers` namespace — the component is
  *conceptual*, NOT one class/file; the no-authority property is structural
  (free fns + value snapshots, no shared mutable field). If any reader grows a
  mutable field, the seam has failed and should re-split. **Internal sub-seam
  fault-lines (kvothe's owner view, offered for sulin's scope call):**
  derivation core (event→AgentInfo→axes/state) is one tight pure cluster —
  leave whole; `task_model` (readTasks + buildTaskGraph over done/triggers/
  tasks-topic) is the cleanest standalone, own tests; `verify` is a one-shot
  claimed-vs-present reader, standalone; **token-scan** is shrinking post
  monitor-truth (tokens now arrive via the statusline-wrapper → `$STATE` file),
  so its future is a *thin `$STATE` reader*, not a heavy transcript scan — the
  tick-eviction stands.
- **Transport thin-fork (HONEST RISK):** the off-TTY arm is *nearly vacuous*
  (the hook pulls), so a Transport component is "ceremony over a two-case fork."
  **Proposed shape:** accept the interface anyway for ONE reason — it quarantines
  the system's biggest doc-lie (the off-TTY-vs-TTY delivery confusion) into one
  named place with one arm-selector (`isOffTty`). Acceptance is conditional: if
  the containment doesn't pay off in clarity, it's *tidy-not-simpler* and should
  collapse back to an `if` in Router. Flag this explicitly for the gate.
  **OUTCOME (2026-06-04, post cut-#2-verdict, tripwire TRIPPED):** examined at
  the gate, the actuation is ALREADY factored — `deliverInline` (TTY arm),
  `isOffTty` (selector), the drain RPC (off-TTY arm), `maybeWakeIdleOffTty`
  (doorbell) — AND already documented (broker-spec §"Delivery model"). A
  `Transport` class would be ceremony over that existing fork, so the
  collapse-to-if fires. The seam's value (quarantine the off-TTY/TTY doc-lie)
  is delivered as DOCUMENTATION, not code: broker-spec §"Delivery model" was
  tightened with the TTY-arm mechanics (sendToPaneSafe + flock + draft-preserve
  + newline-flatten), the comms-is-the-lone-TTY-agent invariant, and an explicit
  "this two-arm model IS the Transport seam, kept as a documented fork" note. The
  multiline-flatten fix (bd58a1e8) is the TTY arm. No `deliver()` component;
  the `if (isOffTty) … else deliverInline` model stands, now authoritatively
  documented.

## 7. The Policy seam — see [docs/policy-actors.md](policy-actors.md)

Policy is the last cut and the only one that *adds a capability*: the
extension point future autonomous behaviors (facts-log distiller, recovery,
coordination hooks) plug into without reopening the seam. The full,
self-contained design + implementation plan — the actor interface, P2 as the
proving actor (no behavior change), and the incremental build sequence — lives
in **[docs/policy-actors.md](policy-actors.md)**, which gates the extraction
(sulin approves it → code lands). One-line shape: `PolicyActor.evaluate(ctx) →
vector<PolicyAction>` (pure), `PolicyAction = Enqueue|Recover|Nudge` with **no
cursor verb** (kernel-untouchable by construction), `PolicyEngine` the
registration surface; P2 (`maybeAutoRecover`) is the reference actor; the
observe/soft/on flag stays orthogonal. dependsOn Router+Readers — a DAG leaf,
hence last.

## 8. Per-seam discipline (every seam)

- **Incremental + behind tests:** each seam lands as its own commit with the
  `Router.plan()`-style fake-backed unit test that proves the cut.
- **Deploy-verify each:** canonical-settle → SIGTERM (broker has the fix) →
  floating relaunch → `bus broker info` build_commit + a delivery smoke. The
  broker delivers mail at EVERY commit — no commit may leave it unable to push.
- **No shared mutable field across a seam** — the relabel test.

## 9. Free hygiene (tracked junk, config-honesty class)

The artifact flags tracked build/scratch junk in the PUBLIC repo against the
repo's own rules: `test_getline.cpp`, `test.txt`, `bin/migrate-state` (a
one-time `/tmp`→XDG migrator, now dead). Removing these is a zero-risk hygiene
commit (same config-honesty class as the `max_record_bytes` removal), landable
independently of the seam work. Verify zero-callers (migrate-state) first.

## 10. Interleave with P2 (locked)

`[P2 breaker on current shape] + [this doc in parallel]` → P2 lands → auri acks
the doc → bottom-up extraction (cuts #1/#2 free; Transport/Readers gated;
**Policy = completed P2, last**). See [[project_p2_auto_recovery]] and
[[project_broker_seam_redesign]].
