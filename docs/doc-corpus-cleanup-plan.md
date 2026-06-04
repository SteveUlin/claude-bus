# Doc-corpus cleanup plan (Phase 4 of the run-2 simplification)

**Status: PLAN ONLY. No deletions until auri clears the kill/keep list past sulin.**
Task `1780458099710-auri-1cae`, Phase 4. Companion to bast's Phase-1 subtraction
(a855ee49) and elodin's broker-code lane.

The redesign's charge: *"~50 design docs and a coordination library whose one
entry is `nothing`"* is part of the weight. This plan turns that into a
verifiable kill/keep/rewrite list, plus the corrected outline for the **one true
`broker-spec.md`**.

## The two load-bearing doc-lies (grep-confirmed spread)

Both contradict landed code. The fleet default is **off-TTY drain**, and the
tick is **timerfd-to-next-deadline**, not a fixed wall-clock loop.

| Lie | Reality | Files repeating it |
|---|---|---|
| "delivery loop **every 250 ms**" (fixed wall-clock tick) | timerfd armed to the next ack-deadline; advanced by RPC activity; 250 ms is a *floor*, not a period | `broker-spec`, `broker-test-sandbox`, `project-roadmap`, `broker-intake-decouple`, `modern-agent-techniques`, `mailbox-design-space`, `delivery-alternatives`, `deep/broker-internals-cpp`, `deep/transport`, `deep/observability`, `deep/comms-patterns` |
| send/broker "**writes into / pushes into the pane**" as the model | off-TTY drain → `additionalContext` is the fleet default; TTY push is the **opt-out / fallback arm** | `improvement-roadmap`, `mechanics-reference`, `broker-spec`, `human-agent-interaction`, `workpaths-proposal`, `delivery-alternatives`, `modern-agent-techniques`, `deep/transport`, `deep/broker-internals-cpp`, `deep/comms-patterns`, `deep/cc-config` |

`broker-spec.md` is the priority rewrite: it is the only doc that mentions
**neither** `off-tty` nor `timerfd` — pure pre-off-TTY, pre-timerfd text, and
it's the one CLAUDE.md sends readers to.

## Disposition (59 docs)

Legend: **KEEP** (true + live) · **REWRITE** (live but lies/stale) ·
**KILL** (served its purpose; truth now lives in code or the spec) ·
**FREEZE** (don't maintain through the refactor; regenerate after Phase-2 shatter) ·
**MERGE** (fold into a survivor, then kill source).
`[ref:N]` = referenced by live code/config N times → killing it requires
updating the referrer in the same change.

### KEEP — living reference / true (8)
- `bus-commands.md` [ref:2] — command catalog. KEEP; audit for dropped subcommands.
- `claude-md-conventions.md` — conventions reference. KEEP.
- `jj-workspace-workaround.md` [ref:1] — live gotcha. KEEP.
- `ultracode-synthesis-pattern.md` — my gap-#18 deliverable, correct + current. KEEP.
- `broker-test-sandbox.md` — test-infra reference (carries the 250 ms phrasing → 1-line fix).
- `external-projects.md` — taro, the live external customer. KEEP.
- `mechanics-reference.md` — **REWRITE-LIGHT**: carries the TTY-push lie; correct the delivery section, otherwise keep.
- `pane-read-audit.md` — the stop-pane-reading migration goal is live. KEEP (or fold into the roadmap).

### KEEP — live design, pending/in-flight work (12)
Not landed yet → the design doc *is* the contract. Revisit when built.
- `broker-auto-recovery.md` [ref:3] — elodin P2, in progress.
- `broker-hardening-batch.md` — elodin's NEXT batch.
- `broker-intake-decouple.md` [ref:2] — Pillar-D, phases ongoing (fix its 250 ms line).
- `orchestrating-state.md` — HOLDING ratify.
- `orchestrator-profile.md` [ref:1] — build held for auri review.
- `research-isolation.md` [ref:2] — blocked on sulin's nixos-rebuild.
- `clear-policy.md` — P3 context-watchdog, not built.
- `p3-trigger-feed.md` [ref:1] — P3, in flight.
- `otel-setup.md` [ref:1] — OTel REC, elodin queued. FLAG: redesign questions OTel-to-`/tmp` (consumed by nobody) — confirm it survives the durable-state move.
- `bus-recover.md` — landed b49549ac but the contract doc is the live reference for elodin's P2 Phase C. KEEP.
- `context-budget.md` [ref:3] — P3-adjacent budget policy. KEEP.
- `task-model.md` [ref:3] — landed; the 3-store reconciliation is non-obvious enough to keep as reference. KEEP (trim to current).

### REWRITE — the one true spec (1)
- `broker-spec.md` [ref:2] — full rewrite. Outline below. Absorbs the
  `broker-lifetime-fix.md` diagnosis so that doc can die.

### KILL — landed design docs, truth now in code + spec (10)
Point-in-time "design → shipped" artifacts. The code is the source of truth;
these are archaeology. Each `[ref]` needs its referrer updated in the same change.
- `broker-gc.md` — landed 86108d86.
- `broker-lifetime-fix.md` [ref:1] — MERGE its PDEATHSIG diagnosis into broker-spec, then kill. (broker-spec currently *links* to it.)
- `dup-delivery-fix.md` [ref:1] — landed 9914d064; fold the 1-line "retry = ack-deadline, not re-deliver" invariant into the spec.
- `status-decouple.md` [ref:2] — landed.
- `monitor-truth.md` [ref:2] — landed (statusline wrapper).
- `monitor-focus.md` [ref:2] — VERIFY landed → kill.
- `computeaxes-fold.md` [ref:1] — landed.
- `log-retention.md` [ref:3] — landed; the retention math belongs in the spec, not a standalone doc.
- `fresh-spawn-delivery.md` [ref:2] — landed; fold any live invariant into the spec.
- `ops-inbox-redesign.md` [ref:1] — superseded by `bus tasks watch` replacing bus-deck; VERIFY → kill.

### KILL — ideation / brainstorm (5)
Served their purpose; the decisions landed. Several are indexed somewhere
([ref]) — update the index.
- `bast-ideation.md` [ref:1], `kvothe-ideation.md` [ref:1], `elodin-ideation.md` [ref:1], `ideation-synthesis.md`, `design-philosophies.md`.

### KILL / MERGE — prior-art surveys (7)
Research scaffolding for decisions already made; many carry both lies.
Recommend: KILL outright. If sulin wants the prior-art kept, fold the still-live
pointers into one thin `docs/prior-art.md` and kill the rest.
- `modern-agent-techniques.md` (629 ln), `human-agent-interaction.md` (577 ln),
  `mailbox-design-space.md`, `delivery-alternatives.md`, `fast-comms-eval.md`,
  `observability-research.md`, `binary-log-formats.md`.

### KILL / MERGE — comms + viewer cluster, superseded by the spec (6)
Overlapping pre-broker comms designs. The broker-spec + bus-commands now own this.
- `comms.md` [ref:1], `comms-routing.md`, `comms-structure.md`, `comms-ui.md` [ref:1] — MERGE any surviving truth into broker-spec/bus-commands, kill.
- `observability-viewers.md`, `observability-research.md` — fold the live viewer list into bus-commands, kill.

### FREEZE — deep-dives, regenerate after the Phase-2 shatter (7)
~4,150 lines that document the **pre-refactor** architecture in depth; several
carry both lies. The redesign shatters `delivery::Loop` into Log/Router/
Transport/Readers — which invalidates these en masse. **Do not rewrite 4k lines
the refactor will obsolete.** Freeze now (stop citing as truth), tag
"pre-refactor", and regenerate a lean set mapped to the surviving components
*after* Phase 2 lands.
- `deep/broker-internals-cpp.md` [ref:1] → regenerate as Log + Router + Runtime.
- `deep/transport.md` → regenerate as Transport.
- `deep/observability.md` → regenerate as Readers (or fold into bus-commands).
- `deep/orchestration.md`, `deep/comms-patterns.md`, `deep/cc-config.md`,
  `deep/memory-learning.md` [ref:2] — likely kill post-refactor; confirm none is the sole home of a live invariant before deleting.

### CONSOLIDATE — roadmaps (2 → 1)
Both carry lies. Keep one corrected roadmap; fold still-open items, kill the rest.
- `project-roadmap.md` (130 ln) — keep as the live roadmap (corrected).
- `improvement-roadmap.md` (435 ln) — harvest still-open items into project-roadmap, kill.

### Prototype dir (separate, already in bast's subtraction scope)
- `docs/prototypes/off-tty-delivery/{README,demo.sh,inbox-drain.sh,sim-broker-write.sh}`
  — the redesign already flags this as tracked scratch in a public repo. Defer to
  bast's elimination list; noting here for completeness.

## Headline
- **KEEP** 20 · **REWRITE** 2 (broker-spec full, mechanics-reference light) ·
  **KILL/MERGE** 28 · **FREEZE** 7 (regenerate post-refactor) · **CONSOLIDATE** 2→1.
- Net: ~30 docs leave the tree now, 7 more after Phase 2. From ~59 to ~22 living docs.
- Every KILL with a `[ref]` carries an explicit referrer-update obligation — no
  dangling links land.

---

## Corrected outline — the one true `broker-spec.md`

Replaces the current 74 lines. Fixes both lies, adds the epoch fence, drops the
dead mailbox path, absorbs the lifetime-fix diagnosis.

1. **What the broker is** — single source of truth for durable delivery. One
   daemon, one processing thread. Owns: topic registry, append-only topic logs,
   per-(topic,consumer) cursors, in-flight tracker, retry/escalation.

2. **The kernel (state this up front — it's the whole design)**
   - Append-only, byte-offset-addressed per-topic **log**.
   - One per-(topic,consumer) **cursor** that advances **only on ACK**.
   - A **boot-epoch stamp** on every record (the field today mis-named
     `SendOpts.correlation`) — the fence that stops a surviving disk log from
     re-delivering yesterday's mail after a restart.
   - Together: at-least-once + dedup + restart-safety. An un-acked record sits
     at the head and is re-evaluated (retry is *free*, not machinery).

3. **Delivery model — CORRECTED.** Off-TTY drain is the **fleet default**: the
   broker marks a record ready, the drain hook injects it as `additionalContext`
   on the agent's next turn. **TTY push** (`sendToPaneSafe`) is the **opt-out /
   fallback arm**, selected by `tty_policy.h`, not the norm. `bus msg send` is the
   separate *raw* lever (direct TUI write, no ACK/retry) — emergency unwedge only.
   Tell the path by the message format, not by habit.

4. **The tick — CORRECTED.** No fixed 250 ms loop. The processing thread arms a
   **timerfd to the next ack-deadline**; RPC activity (viewer polling, sends)
   also drives ticks; **250 ms is a floor**, not a period. Idle ticks are not
   guaranteed — delivery progresses on RPC activity or an armed deadline. (Tests
   must poke an RPC to drive a tick.)

5. **Topic kinds** — keep the existing table (`agent-inbox`, `tui-commands`,
   `work-queue`, `pubsub`, `blackboard`, `append-log`); verify each line against
   current code.

6. **Cursor semantics by kind** — keep (already correct): inbox/tui advance on
   ACK from `events.jsonl` (`UserPromptSubmit` / `Stop`); work-queue on fetch;
   blackboard latest-wins; pubsub per-subscriber.

7. **Presence gate** — `[bus-attach]` sentinel defers records while presence is
   fresh; FIFO, no per-record bypass. (Keep.)

8. **Reliability** — in-flight file per dispatch; on ack-deadline miss, retry
   (retry = re-arm the ack deadline, **not** re-deliver — the dup-delivery fix);
   after N attempts, escalate to `audit` + `inbox-human`, then advance past the
   record so the queue drains.

9. **Lifetime & launch contract** — direct zellij child; `PR_SET_PDEATHSIG`
   ties broker lifetime to the pane; never `nohup`/`setsid`/`disown`. **Absorb
   the `broker-lifetime-fix.md` diagnosis here** (orphan holds the singleton
   flock + DEFER-blocks new sessions) so that doc can be killed.

10. **State layout** — `$STATE/topics.json`, `$STATE/topics/<name>.log` (v4
    wire), `$STATE/cursors/...`, `$STATE/in-flight/...`. **DELETE** the stale
    `/tmp/claude-bus/mailbox/<name>.log` line — `bin/mailbox` no longer exists.
