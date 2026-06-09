# Agent Contract

How the bus learns an agent is READY and DELIVERS a turn — through a defined file/RPC interface, not by scraping the Claude TUI footer. The contract is stated as an obligation the agent meets *by whatever means its runtime allows*; Claude Code and cursor-cli are two realizations, and a third shape (a persistent headless daemon) must be able to plug in without changing the broker.

---

## 1. Problem & thesis

Today the broker learns readiness by guessing at pixels. Every delivery tick can fork `zellij action dump-screen --ansi`, strip ANSI, and parse the Claude footer for `-- INSERT --`, `bypass permissions on`, and an empty input row below the `❯` divider. Three reads sit on the *delivery* critical path: `isAgentIdle` (the `deliver_when=idle` gate), `wakeReadyForMail` (the doorbell's boot-ambiguity branch), and `maybeWakeIdleOffTty` (the zellij fork the cost-discipline comment apologizes for). This is fragile by construction, and the fragility is architectural, not a bug we can patch:

- **It reads a render surface as if it were an API.** The footer is a human display. Its layout, the SGR dim attribute that separates draft from ghost-suggestion, the literal strings `INSERT`/`bypass permissions` — none of it is a contract. A Claude TUI restyle silently breaks readiness.
- **It serializes the broker on a blocking subprocess.** Every pane read forks zellij on the single delivery-loop thread. Fan out across a fleet and the reads stack into tens of seconds, the recv-queue backs up, and the broker stops `accept()`ing — the saturation wedge the doorbell already guards against with a 5 s rate-limit and an event-only-first classify.
- **It only works for one agent type.** Footer-parsing assumes Claude Code's Ink TUI. A second agent — cursor-cli, which runs headless and emits NDJSON — has no footer to scrape. You cannot host a heterogeneous fleet on a Claude-shaped pixel parser.

The fix is one move: **make the agent the sole author of its own readiness, and make the broker a reader of a defined signal.** This unifies three goals that look separate but share a root:

- **Stop pane-reading** (sulin's standing goal) falls out directly — the readiness path reads a `$STATE` file, the pane survives only where pixels are genuinely needed (the comms TTY arm, the monitor-freshness `state` RPC).
- **Reliability** improves because the signal is structured and staleness-fenced — a crashed agent fails *closed* (not-ready) instead of leaving a lying footer, and the broker never blocks on a subprocess to decide whether to deliver.
- **A second agent type** becomes possible because the contract is defined in terms an agent *emits*, not pixels the broker *parses*. cursor-cli implements the same signals through its process lifecycle and NDJSON stream.

**Thesis: the bus already has the right shape hiding inside it.** Off-TTY agents — the fleet default — already declare turn-boundaries (the `SessionStart`/`Stop`/`UserPromptSubmit` hooks fire into `events.jsonl`) and already *pull* their inbox (`bus msg drain` → `additionalContext`). The broker never types into them. The only thing the broker does not yet get from a declared signal is *readiness*: the residual footer scrape that disambiguates the boot window. So the contract is not a new abstraction grafted on top — it is the existing pull model promoted to law, plus one new sentinel that closes the last pane read. TTY-push (comms) is demoted from "a parallel delivery system" to "the one documented exception where a human attaches to the pane."

---

## 2. The contract

**The contract is an obligation, not a mechanism.** It states *what* the agent must make true; each agent type chooses *how*.

> **The readiness obligation.** Whenever the agent parks at a turn boundary — ready to consume its next turn — it writes `$STATE/ready/<name>.json` atomically, by whatever means its runtime allows. The broker only ever reads this file; the agent is its sole author.
>
> **The delivery obligation.** At each turn boundary the agent *pulls* its inbox via `bus msg drain <name> <event>`, folds every returned record into its next turn, and emits exactly one `bus-ack` event per delivered `msg_id` to `events.jsonl` — but only after the record is committed to the agent's input. The broker advances a topic cursor solely on the ack event, never on the drain RPC returning.
>
> **The boundary obligation.** The agent appends a turn-boundary line to `events.jsonl` (the lifecycle vocabulary `SessionStart`/`Stop`/`Notification`/`UserPromptSubmit`/`PreCompact`) so the broker can derive Process/Turn/Mail axes from events alone.

Hooks (Claude) and process-exit (cursor) are **realizations** of these obligations, not the obligations themselves. This is the discipline that lets a third shape — a persistent headless daemon, e.g. an Agent-SDK `query()` loop holding a socket — implement the same contract by writing the sentinel from inside its own loop. The "signals" below name the obligations; §3 and §4 show two realizations.

### The readiness sentinel — `$STATE/ready/<name>.json`

The one new artifact. The agent is its **sole author**; the broker only reads it.

```
{ "session_id": "<opaque>", "boundary_seq": <int64, strictly monotonic>, "ts_ms": <int64> }
```

- **Presence + freshness = "process is up and parked at a turn boundary."** Subsumes the `paneId` liveness check *and* the INSERT-mode disambiguation in one stat.
- **`boundary_seq`** is a per-agent monotonic counter, not a mode string. It lets the broker distinguish "a *new* boundary happened since I last delivered" from "the same prompt I already rang" — the freshness problem a bare touch-file cannot solve. **It must survive relaunch** (see *Monotonicity across restart* below).
- **Derive-don't-store.** The file carries no state the broker would trust an agent to keep honest. `ready` is *derived*: present AND `ts_ms` not older than the agent's last `events.jsonl` event AND `now - ts_ms` within `READY_TTL`. A stale sentinel is treated as **absent** — a crashed agent that never cleared its file self-corrects, with no cleanup hook required.
- **Hard to misuse.** The filename derives from launcher-set `$CLAUDE_BUS_AGENT_ID`, never an agent-chosen string. No body, no routing, no peer-addressing. An agent physically cannot write into another's readiness.
- Written atomically (temp + rename), exactly as `statusline.sh` already writes `$STATE/statusline/<name>.json`. This is a proven shape, not a new mechanism.

**The TTL is doing real work — name it.** The two-clause "stale relative to last event" fence has a hole that matters for *both* agent types and is fatal for cursor: a hung agent with a live wrapper produces **no new event AND no new sentinel**, so the two stay mutually consistent (both old) and only the *absolute* TTL `now - ts_ms` catches it. So `READY_TTL` is not a belt-and-suspenders backstop — it is the *primary* hang-detector. Default proposal: `READY_TTL = 90 s` (a turn boundary should re-stamp the sentinel well inside this; a turn that runs longer than 90 s without re-stamping is indistinguishable from a hang and *should* read not-ready). The clock-jump caveat below governs which clock stamps `ts_ms`.

### Monotonicity across restart — the part that bites

`boundary_seq` must be **strictly monotonic across the agent's entire lifetime**, not just within one session — otherwise the "is this a NEW boundary since I last delivered?" logic (the whole reason to prefer `boundary_seq` over a touch-file) breaks at exactly the resume boundary this design exists to fix. Two viable shapes, sulin's call (see open questions):

- **Persist the counter.** Don't remove the sentinel on `SessionEnd`; on the next boundary, read the prior `boundary_seq` and increment. A crash still leaves a stale (absent-by-derivation) sentinel, but the counter survives.
- **Drop `boundary_seq` for a monotone the broker already trusts** — the `events.jsonl` byte-offset (or last-event sequence) the broker already reads for the freshness fence. This needs no new counter and is crash-robust by construction; the cost is the sentinel no longer self-describes its own boundary, so the broker joins it against the event log it already scans.

**Do not lean on `ts_ms` wall-clock for monotonicity.** The documented suspend/resume wall-clock jump already fires spurious STUCK alarms fleet-wide; a wall-clock `boundary_seq` substitute would inherit that failure mode. Stamp `ts_ms` from `CLOCK_MONOTONIC`-derived time if it is used for the TTL fence, or accept that a resume clock-jump transiently reads agents not-ready (benign — it recovers on the next real boundary).

### Operations

| Op | Who | Signature sketch | Purpose |
|---|---|---|---|
| **`reportReadiness`** | agent emits | writes `$STATE/ready/<name>.json` atomically at each turn boundary | The one new signal. Replaces every footer read on the readiness path. A *realization* of the readiness obligation. |
| **`declareTurnBoundary`** | agent emits | appends one `events.jsonl` line (`SessionStart`/`Stop`/`Notification`/`UserPromptSubmit`/`PreCompact`) | Already exists for Claude. The broker derives Process/Turn/Mail axes from these alone. |
| **`drainInbox`** | agent pulls | `bus msg drain <name> <event>` → up to `kDrainCap=16` records → fold ALL → `additionalContext` (Claude) or next prompt (cursor) | Already exists, and is THE delivery primitive. The agent pulls at its boundary; the broker never pushes (off-TTY). |
| **`ackTurn`** | agent emits | one `{event:bus-ack, payload:{msg_id}}` per delivered record, AFTER commit; `scanEvents` advances the cursor | Already exists for Claude (inside the drain RPC). At-least-once is preserved by construction: the cursor advances on the ack EVENT, never on the drain RPC returning. |
| **`ringDoorbell`** | broker | `maybeWakeIdleOffTty`: wake an off-TTY agent with queued mail IFF `readyFresh(name)` | The broker's only proactive action — and it only applies to a *running, idle* process (Claude pane). cursor's wake is the wrapper's poll loop (§4), not this. |
| **`launch`** | launcher | `agent-launch <name> [--type claude\|cursor] [--project-dir D]` | Sets identity, materializes type-appropriate hooks, writes the initial sentinel + epoch, exec's the agent body. |

**Delivery is a PULL the agent performs, not a PUSH the broker performs.** The broker's role collapses to "answer drains, and ring the doorbell at *running* off-TTY agents whose sentinel is fresh and whose inbox has queued mail." `zellij dump-screen` survives only as (a) the comms TTY exception and (b) a monitor-only freshness read in the `state` RPC.

---

## 3. Implementation #1 — Claude Code (pane)

Claude Code already implements the pull model. This design **adds exactly one hook** and demotes the three pane-read survivors. Everything else is reframing, not rewriting.

| Op | Realized by | Change |
|---|---|---|
| `declareTurnBoundary` | `log-event.sh` on `SessionStart`/`Stop`/`Notification`/`UserPromptSubmit`/`PreCompact` | **None.** `readAgents`/`computeAxes` already derive Process/Turn/Mail from `events.jsonl`, taking `PaneState` only as an injected value. |
| `reportReadiness` | **NEW** `ready-write.sh` | The only added hook. |
| `drainInbox` + `ackTurn` | `inbox-drain.sh` → `bus msg drain` → the RPC folds up to 16 records into `additionalContext` and emits one `bus-ack` per id (`sub_consume.cpp`) | **None.** The post-`/compact` skip stays (a delivery-timing fence). |
| TTY-push (comms) | `sendToPaneSafe` / `deliverInline`, draft-preservation + bypass scan | **None.** Survives ONLY for comms (+ `$CLAUDE_BUS_TTY_AGENTS`). |
| context/effort | `statusline.sh` → `$STATE/statusline/<name>.json` | **None.** Already the file-contract prototype; reused as precedent — and the parity baseline cursor must match (§4). |

### The new hook: `ready-write.sh`

Wired on `Stop` + `Notification(idle_prompt)` + `SessionStart`, removed-or-not on `SessionEnd` per the monotonicity decision (§2). Writes `$STATE/ready/<name>.json` with `session_id` (from hook stdin), the boundary monotone (`boundary_seq` read-and-incremented from the prior file, or the event-offset substitute), and `ts_ms`, atomically.

**Why wire it on `SessionStart` too, not just `Notification`?** This is the crux. `Notification(idle_prompt)` does **not** fire reliably on `SessionStart source=resume` or `source=clear` — which is precisely why the INSERT-mode pane read exists today (the comment at `agent_status.cpp:469-473` documents it). Wiring `ready-write.sh` on `SessionStart` gives the broker a reliable "at a boundary" signal on every resume/clear path, retiring the footer fallback the doorbell apologizes for.

**The compact carve-out — do not let `ready` re-admit the swallow race.** `SessionStart source=compact` must NOT mark the agent `ready` in a way that re-opens the doorbell-compact strand: today `inbox-drain.sh` deliberately skips draining on `source=compact` (the post-compaction `additionalContext` is swallowed, so draining there would consume mail into a void), and the broker's defer-gate keeps a compacting agent as `process=Compacting`. `ready-write.sh` must mirror that: on `source=compact` it either does not write the sentinel, or writes it with a marker the broker reads as "compacting, not idle." The migration's empirical check (§6) must cover the compact path, not just resume/clear — a `ready` sentinel that fires on `source=compact` would re-introduce the wake-then-swallow race the carve-out exists to prevent.

### The broker changes (the shippable win)

Replace the three acquisition sites with reads of the sentinel. The derivation core is untouched — these functions already take their pane snapshot as an *injected value* behind the `pane_state.h` boundary, so we change *what value is injected*, not the derivation.

- **`src/delivery.cpp` `isAgentIdle()` (line 282)** — drop `paneStateCached(agent)`. Read `readReady(agent)`. Idle iff `computeState==Idle` OR (`Starting` AND `readyFresh(agent)`). Liveness (`pane.ok`) → sentinel presence (lifecycle matches `$STATE/agents/<name>.json`).
- **`src/delivery.cpp` `maybeWakeIdleOffTty()` (lines 1403-1413)** — replace the boot-ambiguous `paneStateCached(name)` fork with `readyFresh(name)`. **The doorbell stops forking zellij entirely for off-TTY agents** — this is the read the cost-discipline comment exists to ration, and the sentinel makes the rationing obsolete.
- **`src/agent_status.cpp` `wakeReadyForMail()` (line 471)** — the `(Starting|Stuck) && pane->isInsert()` rescue branch keys off a `ready-fresh` bool the caller derives from the sentinel. The function stays pure; the injected value changes.

New file **`src/ready.h` / `ready.cpp`**: `readReady(name) -> std::optional<Ready{session_id, boundary_seq, ts_ms}>` + `readyFresh(name, now, ttl)`. Pure `$STATE` file read, no zellij. It belongs **behind the `pane_state.h`-style value boundary** — Readers consume the value, never fetch it — so a stray acquisition is a compile error, identical discipline to `PaneState`.

What stays on pixels: `sendToPaneSafe`'s draft-preservation + bypass scan (comms-only, genuinely needs the footer) and the `state` RPC's raw `paneState()` (monitor freshness, explicitly not the delivery path). **Net for Claude: +1 hook script; the doorbell and idle-gate stop forking `dump-screen` for every non-comms agent.**

---

## 4. Implementation #2 — cursor-cli (headless)

cursor-cli is the proof the contract is type-agnostic — and the place every glossed-over corner gets exercised. It is **unconditionally off-TTY** — there is no human attaching a headless pipe, so no TTY exception applies. It realizes the obligations through a thin per-turn wrapper around `agent -p`, not a persistent injectable process. The wrapper IS the cursor agent body; `agent-launch --type cursor` exec's it.

cursor-cli fits the contract *better* than Claude in one dimension: it is inherently turn-scoped. One `agent -p '...'` invocation = one turn; **process exit IS the turn boundary** — a cleaner ready signal than any footer. It fits *worse* in three: no statusline (context/effort blind), a lossier `events.jsonl` vocabulary, and no in-flight steering. §4 spells out all three rather than claiming a parity it can't deliver.

### Launch, auth, isolation — the part that gates shippability

`agent-launch --type cursor` pre-declares `--force --trust` (the bypass equivalent; without `--force`, cursor-cli only *proposes* file changes), then exec's `cursor-turn-loop <name>`. Identity = workspace cwd + name. Session continuity = `session_id` captured from the `system/init` NDJSON event, persisted to `$STATE/cursor/<name>.session`, threaded as `--resume=<id>` next turn. **Track the explicit `session_id`; never use `--continue`/`--resume=-1`** — its cwd-scoping is unverified, and two cursor agents fighting over "most recent" is the failure mode.

The auth + isolation story is a **launch-contract requirement, not a detail** — without it Slice 4 is not launchable:

- **`CURSOR_API_KEY` source.** Headless **requires** it (no browser popup in a pane). The repo is **public** (`CLAUDE.local.md`: no secrets in tracked files), so the key must come from an **untracked** source: `.env`/`.envrc.local` (already gitignored) read by `agent-launch`, or the host keyring/env. It must never land in tracked config, layouts, or hooks.
- **One account key ⇒ one rate-limit/billing context, fleet-wide.** Every cursor agent shares the subscription's rate-limit and cost pool. The parallelism/limit-cadence gauge is therefore **per-account, not per-agent** for cursor — N cursor agents draw on one bucket. Tune accordingly; this is a different shape from Claude's per-agent context.
- **SEC-1 interaction.** If a cursor agent runs inside the netns/squid cage, the squid CONNECT allowlist **must admit cursor's API backend** (cursor.com's API host), or every turn dies in the SYN-SENT hang. A cursor agent outside the cage needs no allowlist change but loses the isolation guarantee — sulin's call which cage tier cursor agents land in.
- **No rate-limit-modal unstick analog.** Claude's recovery ladder (raw Enter to dismiss `/rate-limit-options`) is pane/TTY-specific and has **no cursor equivalent**. A rate-limited cursor turn surfaces as a failed/timed-out `agent -p` (handled by the per-turn timeout below), not a modal — recovery is respawn, not keystroke.

### The turn loop (one iteration = one turn)

The drain-and-ack protocol is stated as an **invariant**, because the naive version silently loses mail:

1. **PULL (batch).** `bus msg drain <name> cursor-turn` returns a `messages[]` array of **up to `kDrainCap=16` records**. Fold **every** record into the turn's prompt (sender + body, mirroring `sub_consume.cpp`'s framing). If empty and an open task exists, synthesize a "continue your work" prompt. Remember the full list of delivered `msg_id`s.
2. **RUN.** `agent -p '<folded-prompt>' --output-format stream-json --force --trust --resume=<session_id> </dev/null`. Parse NDJSON. The `result`/`subtype:success` event + exit 0 IS turn-success.
3. **ACK — only on confirmed turn-success, per id.** ON `result.subtype:success` AND exit 0, append **one** `{event:bus-ack, payload:{msg_id}}` line to `events.jsonl` **for each** delivered `msg_id` — exactly the per-id loop `sub_consume.cpp` runs inside the drain RPC. **Never ack at fold-time.** If `agent -p` times out (the SYN-SENT hang) or exits non-zero, emit **no** acks — the cursor never advanced, so the next turn re-drains the same records and re-delivers. This is the at-least-once guarantee; acking before the process consumes the prompt is the exact silent-loss failure the invariant forbids.
4. **`reportReadiness` + `declareTurnBoundary`.** On process exit, write `$STATE/ready/<name>.json` `{session_id, boundary_seq-or-offset, ts_ms}` atomically. Because cursor-cli runs **no** Claude hooks (and `afterAgentResponse` doesn't fire in `-p`), the **wrapper itself** appends the boundary lines to `events.jsonl` (mapping `system/init`→`SessionStart`, `result`→`Stop`).
5. **Loop**, blocking on the cursor doorbell substitute below until queued mail or an open task makes the next pull non-empty.

**Why the ack window is NOT identical to Claude's.** Claude's `bus-ack` rides the *same* drain RPC step that commits the `additionalContext` (atomic within one hook invocation). cursor's wrapper splits fold (step 1) and ack (step 3) across a process boundary known to hang. The invariant — ack strictly after `result:success` + exit 0 — is what makes the cursor window safe: a crash anywhere between fold and confirmed success re-delivers, never loses.

### The cursor doorbell — polling, stated honestly

The broker **cannot** push a wake into a non-running process. The Claude doorbell (`maybeWakeIdleOffTty` → `deliverInline('[bus-wake]')`) is therefore **Claude-only**. For cursor, **the wrapper polls the inbox on a bounded interval between turns** (chosen over a broker-written `$STATE/wake/<name>` poke-file because the poke-file would be a *new control channel the broker writes and the agent selects on* — precisely the new IPC §8 forbids). The poll resolves the §8-vs-§4 tension by choosing the mechanism that adds no broker-written channel:

- **Mechanism:** between turns the wrapper sleeps `CURSOR_POLL_INTERVAL` (proposed 2 s), then re-drains; non-empty ⇒ run a turn, empty ⇒ sleep again.
- **Worst-case mail latency:** `current-turn-duration + CURSOR_POLL_INTERVAL`. Mail that arrives mid-turn waits for the whole turn (unbounded by turn length) plus one interval. This is a real, stated cost — not the prompt wake Claude gets.
- This is a **polling fallback, not the doorbell**. §5 reflects this: Doorbell is "Claude-only; cursor uses bounded polling," not "preserved."

### Steering a cursor agent — a coarser gate, flagged

For Claude the human attaches to the pane and types. For cursor there is **no pane** (correctly — it's headless) **and no in-flight injection**: a long-running or hung `agent -p` (the documented 10–30 s SYN-SENT hang, plus `AskQuestion`'s undocumented headless fate) **cannot be steered or interrupted mid-turn**. The human's only lever is **process-kill/respawn**. So "single human gate" is *preserved in spirit* (the human still steers only through the bus) but the gate is **coarser for cursor than for Claude** — kill vs. attach-and-type. This is a genuine reduction; it is named here, not buried.

### Observability degradation — named, not papered over

Three observability surfaces degrade for cursor. The contract scopes cursor as **observability-degraded** rather than claiming parity:

- **No statusline ⇒ context/effort blind.** `statusline.sh` → `$STATE/statusline/<name>.json` is the **only** authoritative source for context-window size + live effort (not in the transcript). cursor has no statusline; the NDJSON `result` event carries `duration_ms` but **not** token/context usage in the fields we pin. Consequence: the **P3 context-watchdog** (which must auto-manage *every* agent) and the monitor's context column have **no data source for cursor**. Partial mitigations to investigate in Slice 4: cursor's `stop`-hook `additional_context`, or a token estimate from `result` text length — but until verified, **cursor is context-blind and P3 cannot manage it**. State this; don't let the monitor lie.
- **Lossier `events.jsonl` ⇒ no tool-level events.** The wrapper synthesizes only `SessionStart`/`Stop` (from `system/init`/`result`). No `PreToolUse`/`PostToolUse`/`PreCompact`. Anything keying on tool events — `focus-write.sh` on `TodoWrite`, the WORKING-vs-tool axis, task-graph spans — **silently degrades for cursor**. cursor 1.7+ *has* real hooks (`postToolUse`, `CLAUDE_PROJECT_DIR` alias), so richer parity is *available* later; it is deliberately **not** load-bearing for readiness, but the degradation is real for observability.
- **No `tui-commands` actuator ⇒ no slash dispatch.** `isAgentIdle`'s `deliver_when=idle` TTY records and `dispatchTuiCommands` (slash delivery, e.g. `/clear` via `mailbox slash`) assume a TTY agent. cursor is off-TTY, so **a whole delivery topic (`tui-commands`) is Claude-only**. cursor's `/clear` analog is "start a fresh session" (drop `--resume`, not type `/clear`); `/compact` has no headless analog. P3's primary Claude lever under rate-limit (`/clear`-via-slash) maps to **wrapper-side session reset** for cursor, not a slash.

### Why the wrapper, not cursor hooks

The **wrapper-only path needs ZERO cursor hooks**, which is far more robust given the documented headless gaps: `afterAgentResponse`/`afterAgentThought` don't fire in `-p`, and `AskQuestion` skips `preToolUse`/`postToolUse`. The NDJSON stream is the contract; cursor hooks are optional polish for richer event-log parity (closing the lossier-`events.jsonl` gap above), never load-bearing for readiness.

### Unknowns carried from research — explicit

- **stop-hook-in-headless** is unverified → we don't depend on it; readiness rides on process exit + the `result` event, both confirmed reliable.
- **`AskQuestion` process fate in headless** (hang vs auto-deny) is undocumented → mitigate with `</dev/null` (stdin closed) + a per-turn timeout; a hung turn surfaces as a missed sentinel re-stamp (TTL → not-ready), the same staleness fence — and crucially the TTL, not the event-relative clause, is what catches it (§2).
- **The `-p` SYN-SENT hang bug** ("largely fixed early 2026," but beta) → the wrapper wraps each `agent -p` in a timeout, treats timeout as a failed turn (no ack), and re-spawns with the same `--resume`.
- **NDJSON schema is beta/unversioned** → pin to the `result` event's documented core fields only (`type`/`subtype`/`is_error`/`session_id`); a parse failure escalates to `inbox-human`, never silent infinite retry.

---

## 5. Invariants preserved (and one explicitly downgraded)

- **At-least-once** — preserved for both types, but by *different* windows, both stated. Claude: the drain cursor advances on the `bus-ack` EVENT (via `scanEvents`), and the RPC emits one ack per delivered id only after the `additionalContext` is committed to stdout (`sub_consume.cpp`). cursor: the wrapper emits one ack per id **only after `result:success` + exit 0**, never at fold-time — so a mid-turn crash or SYN-SENT timeout re-delivers the whole batch. `noteDrainDelivery`'s `next_retry_at=0` (off-TTY records are never TTY-re-dispatched) is unchanged. The sentinel is **advisory** (when to push), never authoritative for whether a turn was consumed — that is the ack.
- **Off-TTY default** — strengthened into THE contract, not just the fleet default. `isOffTty()` logic is byte-for-byte unchanged; cursor agents are off-TTY because they aren't comms. The broker never types into a pull agent.
- **Comms TTY exception** — preserved exactly. comms stays the compiled-in bedrock opt-out in `tty_policy.h`; `sendToPaneSafe` + draft-preservation + bypass/INSERT footer scraping survive **only** for comms (+ `$CLAUDE_BUS_TTY_AGENTS`). The design removes footer reads everywhere *except* this exception; it does not touch the exception.
- **Doorbell — Claude-only; cursor uses bounded polling (DOWNGRADE, stated).** For Claude the doorbell is preserved and de-fragilized: `maybeWakeIdleOffTty` still rings idle off-TTY agents, with the readiness GATE moved from a `dump-screen` fork to a sentinel stat (no zellij, no saturation-wedge). For cursor it is **replaced by the wrapper's bounded poll** (§4) — a non-running process cannot be pushed to. Worst-case cursor mail latency is `turn-duration + poll-interval`. This is a real downgrade for cursor, recorded here rather than mislabeled "preserved."
- **Presence / focus** — untouched. `hasPresenceFile()` remains the ONLY mail-suppression signal; both `dispatchAgentInbox` and the drain RPC gate on it. cursor agents have no human pane → presence is always-absent (correct: nothing to fight over).
- **Single-human-gate — preserved in spirit, coarser for cursor (stated).** For Claude the human still attaches to any pane and types. For cursor the human steers only by mailing through the bus *and* cannot interrupt an in-flight turn — the only mid-turn lever is process-kill/respawn (§4). The gate holds (no side-channel) but is coarser for headless agents; this is a reduction, named.
- **Config-via-land-to-main** — preserved. `ready-write.sh`, the cursor wrapper, and any cursor hooks materialize from landed `main` through `agent-launch`'s existing mechanism. Config still propagates by land + relaunch, never live edits. The `CURSOR_API_KEY` source is the one exception that must stay **untracked** (§4).
- **Readers value boundary** — preserved. `ready.h` sits behind the `pane_state.h`-style seam: derivation code consumes `Ready` as an injected value and never fetches it; the broker loop acquires it and passes it in.

---

## 6. Migration path

Every step keeps the broker delivering mail. The pane read is **demoted, then deleted** — never removed before the replacement is proven.

**Slice 1 — the first shippable win: move the `deliver_when=idle` gate off the pane.** Ship `ready-write.sh` (Claude) + `ready.h/cpp` + change `isAgentIdle()` to read the sentinel with a `CLAUDE_BUS_PANE_FALLBACK` flag — if the sentinel is missing, fall back to today's `paneStateCached`. Self-contained, one gate, verifiable: flag on ⇒ behavior identical; flag off ⇒ the gate reads the file. Land it, relaunch, watch the fleet deliver. **This is the proof the contract works before we lean on it.**

**Slice 2 — the doorbell.** Once Slice 1 has run clean across a fleet relaunch, move `maybeWakeIdleOffTty` and `wakeReadyForMail` onto `readyFresh`. This deletes the zellij fork the cost-discipline comment guards — the highest-leverage reliability gain. Keep the fallback flag.

**The empirical gate between Slice 1 and Slice 2 — three checks, not one.** Before leaning on the sentinel for the doorbell, validate across a real fleet relaunch:

1. **`SessionStart` fires the sentinel reliably where `Notification(idle_prompt)` doesn't** — `source=resume` and `source=clear`. This is the crux the whole design rests on; if `SessionStart` is also unreliable, the footer fallback can't be deleted.
2. **`source=compact` does NOT mark the agent `ready`** — the sentinel must stay "compacting, not idle" on the compact path, or it re-opens the doorbell-compact swallow race (§3). Verify a compacting agent is not woken into the swallowed-`additionalContext` void.
3. **`boundary_seq` (or the event-offset substitute) is strictly monotonic across the relaunch**, not reset to 1 — the resume boundary is exactly where naive "read prior file, increment" breaks (§2).

**Slice 3 — delete the fallback.** After Slices 1-2 soak, remove `CLAUDE_BUS_PANE_FALLBACK` and the dead pane reads on the readiness path. The footer scrape survives only in the comms TTY arm and the `state` RPC. Stop-pane-reading achieved for the off-TTY fleet.

**Slice 4 — cursor-cli.** Greenfield, gated behind `--type cursor`, zero risk to the Claude fleet. Build `cursor-turn-loop` + `agent-launch --type cursor`. Validate against a live cursor-cli on the §4 unknowns: `session_id` capture, batch-drain-as-next-prompt + per-id ack-after-success, the hang timeout, the `CURSOR_API_KEY`/SEC-1 wiring, and the observability-degraded scoping (monitor must show cursor's context column as "n/a," not a guess). This is the genuinely new code; the contract above is what lets it plug in without touching the broker's delivery core.

**Ordering rationale:** Slices 1-3 are the *contract*; Slice 4 is the *second implementation*. Prove the contract on the agent type we already run before betting the fleet on a beta CLI.

---

## 7. Open questions for sulin

See the recommended-decisions block. The load-bearing ones: which `boundary_seq` monotonicity shape (persist the counter vs. reuse the event-offset); the `READY_TTL` value (and whether a hung-but-running cursor turn reading not-ready at 90 s is acceptable); whether the empirical gate must clear all three checks (resume/clear, compact, monotonicity) before Slice 2; which SEC-1 cage tier cursor agents land in and how `CURSOR_API_KEY` is provisioned; and whether cursor-cli ships now (Slice 4) or stays a design-only stub until the Claude contract has soaked. The observability degradation (cursor is context-blind for P3, tool-event-blind for focus/task-graph, slash-less for `tui-commands`) is also sulin's call to accept-as-scoped vs. block Slice 4 on closing it.

---

## 8. Out of scope / explicitly NOT doing

- **No new IPC daemon, named pipe, or RPC the agent must *call* for readiness — and no broker-written poke-file.** The sentinel is a file the agent writes (mirroring `statusline.sh`); the broker stats it. The cursor doorbell substitute is the **wrapper's own poll** (§4), deliberately chosen over a broker-written `$STATE/wake/<name>` channel precisely so this rule holds: the broker writes no control channel the agent waits on, and the poll's worst-case latency is stated rather than hidden behind a new mechanism.
- **No MCP for turn delivery.** MCP is a tool-*exposure* protocol — it inverts control flow (agent pulls by calling a tool). The drain RPC + sentinel already give pull-first cleanly. Reserve MCP for agents that *offer* tools to the broker.
- **No `claude --bg` background-supervisor migration.** Claude's agent-view supervisor (`claude agents --json` + peek/reply) is an alternative non-TTY injection path, but it's a different architecture (per-session supervisor process) and its reply mechanism has no confirmed machine-readable CLI. Out of scope; the pull model gets us there without it.
- **No HTTP hooks** (`type:http` POSTing events straight to the broker). Attractive for eliminating shell-fork overhead, but it collides with the SEC-1 netns/squid allowlist and is an optimization, not a contract requirement. Defer.
- **No stuck-state recovery rework.** The contract covers steady-state readiness + delivery. The recovery ladder (raw Enter to dismiss `/rate-limit-options`, etc.) stays pane/Claude-specific for comms-class agents; cursor's recovery is process-kill/respawn (it has no modal to dismiss), a separate story.
- **No bypass/permissions sentinel for off-TTY agents.** `bypass_perms` is read only inside `sendToPaneSafe` (comms-only). Off-TTY agents never traverse that path; cursor pre-declares `--force --trust` at launch. The contract needs no bypass signal.
- **No removal of `events.jsonl` or the lifecycle hooks.** They stay the cross-agent event log and the Process/Turn/Mail derivation spine. The contract adds an *explicit* readiness sentinel beside them; it does not replace the event log.
- **No observability parity for cursor in v1 — scoped, not promised.** cursor ships **observability-degraded** (§4): context/effort-blind (no statusline ⇒ P3 cannot manage it), tool-event-blind (lossier `events.jsonl` ⇒ focus/task-graph degrade), slash-less (no `tui-commands` actuator). Closing these (cursor `stop`-hook `additional_context`, real cursor hooks for tool events, `--resume`-reset as the `/clear` analog) is deferred follow-up, not a v1 deliverable. The monitor must render cursor's missing columns as "n/a," never a guess.
