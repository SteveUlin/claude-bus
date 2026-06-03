# Broker auto-recovery triage (P2 · R1)

**Status:** design — surfaced to auri for ack before implementation.
**Owner:** elodin. **Scope:** `src/delivery.{h,cpp}` (generalize `maybeAutoClear`
into a triage engine), a small recovery-ledger state file. **Touches
restart/delivery semantics** — design-first per the "design before code on
restart/ack/cursor" rule. An auto-relaunch that misfires *kills a working
agent's session*, so the safety bar here is the highest in the broker.

## 1. The frame

The broker already has **detection** and one **action**, wired ad-hoc:

- **Detection (observability-only):** `maybeEscalateStuck` (D8 Part B) emits
  audit alarms — `turn-stuck` (a turn open past budget), `tool-wedged` (a
  PreToolUse with no PostToolUse past budget) — and the strand watchdog flags
  stranded off-TTY mail. These *describe* a problem; nothing acts on them.
- **Action (one, hard-coded):** `maybeAutoClear` enqueues `/clear` to an idle
  worker past an idle threshold. A single signature → single action, with its
  gates inlined.

The landscape research (`project_reliability_priorart`) names **liveness-driven
auto-recovery as the single biggest gap** — peers restart on a heartbeat; we
detect but don't recover. P2 closes it by turning recovery into a **data-driven
triage table**: a list of `{signature → action → guard}` rows the engine
evaluates each scan. `maybeAutoClear`'s idle→`/clear` becomes one row;
`maybeEscalateStuck`'s alarms become signatures that now carry actions.

## 2. Goals / non-goals

**Goals**
1. One **declarative table** of recovery rules, not scattered `if`-ladders. New
   failure modes are new rows, not new functions.
2. **Recover, don't just alarm:** nudge / `/clear` / relaunch / escalate-human,
   chosen per signature, with an escalation ladder.
3. **Safety first** — never kill a slow-but-working agent. Every relaunch
   requires **two independent signals agreeing**; all staleness timers read the
   **monotonic clock** (suspend/resume immunity, `project_suspend_resume_wallclock`);
   a thrash circuit-breaker (OTP MaxR/MaxT) + per-signature exponential backoff
   bound runaway recovery; a global kill-switch disables the whole engine.

**Non-goals**
- **Not a new daemon / thread.** Runs as a rate-limited step in the existing
  delivery tick (now self-driven post-P1), same shape as `maybeAutoClear`.
- **The broker does NOT own the relaunch primitive.** Respawning a pane /
  `claude` session is `bus spawn` / `agent-launch` — **bast's** domain. The
  triage engine *invokes* it (shells `bus spawn`/agent-launch or a zellij
  action); it does not reimplement it. Boundary stays clean (§6).
- **Output-verification (artifact-missing) is P5's** (verify-before-trust). The
  `context-100% → /clear` row depends on it; until P5 lands, that row ships in
  a degraded form (fill-% heuristic only) or observe-only. Linked, not blocked.
- **No wire-format / cursor / ack change.** Recovery actions reuse existing
  mechanisms (enqueue to `commands-<agent>`, mail `inbox-ops`, doorbell submit).

## 3. The model: signature → action → guard

```
for each live agent, each scan:
  for each row in TABLE:
    if row.signature(agent) matches      // a predicate over signals
       and row.guard.allows(agent, row): // backoff + MaxR/MaxT + 2-signal
      row.action(agent)                  // nudge | clear | relaunch | escalate
      ledger.record(agent, row)          // for backoff + thrash accounting
      audit(agent, row)                  // always observable
```

- **Signature** — a pure predicate over the signals the broker already has:
  `AgentInfo` fold accumulator (`turn_start_ms`, `open_tool`,
  `open_tool_since_ms`), `computeAxes` (Process/Turn/Mail/Tui), pane state,
  transcript mtime **and content-progress**, token-fill % (from
  `maybeScanTokens`' `$STATE/status/<agent>.json`), inbox depth, in-flight set,
  and error markers in `events.jsonl`.
- **Action** — a recovery verb (§4).
- **Guard** — the anti-thrash + safety layer (§5). The action fires only if the
  guard allows.

## 4. Actions (the recovery ladder, softest → hardest)

Actions are ordered by blast radius. A signature names its *entry* action; on
repeated failure the engine escalates to the next rung (the "nudge ≤1–2× then
relaunch" pattern), governed by the guard.

1. **nudge** — a single doorbell-style sentinel submit (`[bus-wake]`) via the
   flock'd safe-write path. Wakes an idle/stalled agent to take a turn (drain
   its mail, resume in-flight work). Cheapest; no state loss. Reuses
   `deliverInline` / the doorbell path.
2. **clear** — enqueue `/clear` to `commands-<agent>` (exactly `maybeAutoClear`
   today). Reclaims context; loses conversation but not the session. For
   context-rot / context-100%.
3. **relaunch** — kill + respawn the agent's `claude` session (resume by stable
   UUID). The heavy action: a fresh process, conversation gone, in-flight work
   must re-deliver. **Invokes bast's spawn/agent-launch primitive** — the engine
   never reimplements it. Highest guard bar (2 signals + MaxR/MaxT).
4. **escalate-human** — mail `inbox-ops` (the existing `escalate()` path) and
   STOP auto-recovery for that agent. The terminal rung: when the circuit
   breaker opens or a relaunch itself fails, hand to the human rather than
   thrash.

## 5. The triage table (initial rows)

Each row: **signature (signals that must agree)** → **action** → **guard notes**.

| # | Signature | Signals (≥2 agree for relaunch) | Action | Guard |
|---|---|---|---|---|
| R1 | **idle-context** — `Turn=Ready`, idle ≥ N min, inbox empty, no in-flight | last_event=Stop + idle-age (monotonic) | `clear` | role-exclusion (comms/primary), 5-min cooldown. *(= today's `maybeAutoClear`)* |
| R2 | **relaunch-idle** — agent `Turn=Ready` but has **queued mail / in-flight not draining** (post-relaunch didn't auto-resume) | mail pending **AND** wake-ready prompt **AND** no recent drain | `nudge` | doorbell cooldown 30 s; this is the doorbell, promoted into the table |
| R3 | **hung-turn** — turn open with **no content-progress** > 2× expected | `turn_start_ms` past budget (monotonic) **AND** transcript byte-offset unchanged | `nudge` ≤1–2× → then `relaunch` | exp-backoff per agent; relaunch needs R-wedged corroboration |
| R4 | **wedged** — process not making progress | transcript-mtime stale **AND** pane **not** awaiting-input (not INSERT-idle, not a modal we expect) | `relaunch` | **2-signal mandatory**; MaxR/MaxT; never on mtime alone |
| R5 | **thinking-block API-400 (#10)** | error-signature in events/transcript persists **≥2 turns** (recurs from parallel-tool-call-erroring) | `relaunch` + **circuit-breaker half-open probe** | after K relaunches → stop + escalate; half-open = one probe before re-arming |
| R6 | **context-100%** — correctness failure, agent alive | fill ≥ threshold **AND** (P5) output-verification: claimed artifact missing | `clear` | **depends on P5**; until then fill-only → observe-only or soft-nudge |

Notes:
- **Token-rate spike (10–100×)** is the most-cited "stuck" signal. Per auri's
  decision (Q3), the derivative Δtokens/Δt is **produced by kvothe's
  token-monitor** (in build now) — this engine **consumes** it, does NOT
  recompute it. Sync the data location/format with kvothe before wiring it as
  R3/R4's second corroborating signal. Until then, Phase A uses transcript
  mtime/byte-offset staleness as the available progress signal.
- **R6 is the HARD auto-clear and is gated on P5** (output-verification). It is
  **distinct from P3's** fill-%@60% soft-nudge-to-NOTES (a separate, safe
  context-watchdog action). Do NOT fold them: fill-% alone is too weak to
  auto-`/clear` on (risks clearing an agent mid-useful-work); only P5's
  claimed-artifact-missing check detects the correctness failure reliably.
- **The 2-signal rule kills the classic false positive:** mtime "lies like a
  PID" — a long thinking/tool call looks stale by mtime but is healthy.
  Pairing mtime-stale with pane-not-awaiting-input (or with token-rate≈0 over
  the window) requires *agreement* before the destructive action.

## 6. Guards — the anti-thrash + safety layer

- **2-signals-agree (the cardinal rule):** any `relaunch` requires two
  independent signals to agree. Single-signal relaunch is forbidden — it's the
  liveness/readiness conflation trap. `nudge`/`clear` (non-destructive) may fire
  on one strong signal.
- **OTP MaxR/MaxT circuit breaker:** ≥ `MaxR` relaunches within `MaxT` for an
  agent (default 3 / 10 min) → **open the breaker**: stop auto-recovery for that
  agent, escalate to `inbox-ops`. Half-open (R5): after a cooldown, allow ONE
  probe; success re-closes, failure re-opens. Mirrors CrashLoopBackOff.
- **Per-signature exponential backoff:** after firing a row's action, back off
  `base × 2^attempts` before the same row can fire for that agent again. Stops a
  flapping signature from hammering.
- **Monotonic clock everywhere:** all age/staleness/window math uses the same
  monotonic clock as W1 — a lid-close/resume wall-clock jump must NOT mass-fire
  recovery (it currently mass-fires false STUCK; recovery acting on that would
  be catastrophic). Recognize-don't-chase, enforced structurally.
- **Global kill-switch:** `CLAUDE_BUS_AUTO_RECOVERY=0` disables the engine
  entirely; per-action env gates (`..._RELAUNCH=0`) disable just the heavy rung.
  Defaults: ship **observe-only** (see §8).
- **Role exclusion + presence:** comms/primary excluded from destructive rows;
  an attached pane (`hasPresenceFile`) defers recovery — the human has it.

### 6.1 Clock handling — the concrete mechanism (Phase B refinement)

§6/§7 say "monotonic everywhere, same care as W1." Making that concrete
surfaced a gap worth flagging *before* coding: `nowMs()` is **wall-clock**
(`system_clock`), and `events.jsonl` is **wall-stamped by hooks**, so age math
(`now − last_event_ms`) is inherently wall-based — a suspend/resume jump
inflates every age → false STUCK/idle. W1/detection tolerates this as *benign*
(recognize-don't-chase, no action) and **never implemented a jump mechanism**.
Recovery **acts**, so it's the first consumer that genuinely needs one. Two
concerns, two mechanisms:

**(a) Wall-jump grace — protects age/staleness reads.** Each tick records a
`(wall, mono)` pair (`mono` = `steady_clock`/CLOCK_MONOTONIC, which *pauses*
rather than *leaps* across suspend). If `Δwall − Δmono > JUMP_THRESHOLD`
(~5 s) between consecutive ticks, a clock jump occurred (suspend, NTP step,
restart-across-reboot) → arm a **suspend-grace window** (~60 s mono) during
which the recovery engine **no-ops entirely**. Every age spanning the jump is
untrustworthy; skip recovery until ages re-stabilize. This is the structural
"recognize-don't-chase" W1 lacked. (The monitor may still flash transient
false-STUCK — recovery just won't act on it.)

**(b) Monotonic ledger windows + boot-id persistence — protects breaker/
backoff.** The ledger's own timestamps (MaxR/MaxT relaunch window,
`backoff_until`, breaker `open_until`) are set by recovery *when it fires*, so
recovery owns their clock → use **monotonic ms**. Persistence:
- CLOCK_MONOTONIC is continuous across processes within one boot, so a **broker
  restart (same boot) preserves the windows** — exactly what "breaker must
  survive restart" requires.
- It resets on **reboot**. So tag the ledger file with the **boot id**
  (`/proc/sys/kernel/random/boot_id`, or `btime` from `/proc/stat`). On load:
  boot_id matches → rebase mono-relative, windows valid; boot_id differs →
  **reset the windows**. Resetting on reboot is SAFE — a pre-reboot crash-loop
  is stale and the reboot was almost certainly human-caused; worst case a
  genuine looper gets a fresh MaxR budget post-reboot, which the human just
  triggered anyway.

Net: (a) stops suspend/resume from firing recovery; (b) makes the breaker
survive a broker restart yet reset cleanly on reboot — "monotonic everywhere",
concretely, for an engine fed by wall-stamped events.

## 7. State — the recovery ledger

A per-agent ledger, persisted to `$STATE/recovery/<agent>.json`, holding:
- per-signature `{last_fired_ms, attempts, backoff_until_ms}` (exp-backoff)
- a rolling list of `relaunch` timestamps (MaxR/MaxT window)
- breaker state `{closed | open | half-open}` + `open_until_ms`

**Persistence is required:** a broker restart must NOT reset the breaker — else
a crash-looping agent that bounced the broker gets infinite fresh relaunches.
Loaded on boot like the in-flight tracker; pruned for agents gone past a TTL.
All windows use the monotonic clock, persisted as monotonic-relative or
re-based on load (same care as W1).

## 8. Rollout — observe-only first, then soft, then hard

Each phase verifiable in an isolated `CLAUDE_BUS_STATE`, never the live broker.

1. **Phase A — engine + observe-only.** Build the table + ledger + guards. Every
   row, on match, **logs what it WOULD do** to audit (`would-recover agent=…
   signature=… action=…`) but takes **no action**. Run against the live event
   stream (sandboxed) to confirm signatures fire on real failures and DON'T
   fire on healthy agents (false-positive shakedown). This is the safety gate.
2. **Phase B — soft actions.** Enable `nudge` + `clear` rows (R1, R2, R3-nudge,
   R6-soft). These are non-destructive; low risk. `maybeAutoClear` is replaced
   by R1 here (behavior-identical).
3. **Phase C — relaunch.** Enable `relaunch` (R4, R3-escalate, R5) behind the
   2-signal rule + MaxR/MaxT, after Phase A proves the signatures are clean and
   bast confirms the relaunch primitive's contract. Ship still gated by
   `CLAUDE_BUS_AUTO_RECOVERY` default — flip to on only after burn-in.

## 9. Why this won't kill a working agent (the safety argument)

- A destructive action (`clear`/`relaunch`) never fires on a single signal —
  two independent signals must agree, and the pair is chosen so a healthy-but-slow
  agent fails at least one (a thinking agent advances transcript bytes / token
  count even with stale mtime; a working agent's pane isn't an idle prompt).
- The monotonic clock makes suspend/resume a no-op for recovery (the one event
  that mass-trips staleness).
- The breaker caps total damage: even a mis-firing signature can relaunch an
  agent at most `MaxR` times before the engine stops and calls the human.
- Observe-only Phase A surfaces every false positive before any action ships.
- Kill-switch gives an instant fleet-wide off.

## 10. Test plan

- **Signature unit tests** (pure predicates, no I/O — like `foldTurnState`
  tests): feed crafted `AgentInfo` + pane + token-fill and assert each
  signature matches / doesn't, especially the 2-signal boundary (mtime-stale +
  pane-INSERT-idle ⇒ NO relaunch).
- **Guard unit tests:** MaxR/MaxT opens after 3/10-min; exp-backoff spacing;
  breaker half-open probe; monotonic-clock window survives a simulated clock
  jump (no mass-fire).
- **Engine itest (observe-only):** seed an events.jsonl with a wedged agent +
  a healthy agent; assert the audit log shows `would-recover` for the wedged
  one and nothing for the healthy one.
- **Ledger persistence itest:** trip the breaker, restart the broker, assert
  the breaker stays open (no reset).
- **Regression:** R1 must reproduce `maybeAutoClear` exactly (existing
  auto-clear behavior unchanged).

## 11. Resolved decisions (auri, 2026-06-01)

1. **Relaunch primitive — bast owns it; do NOT block (Phase A acts nothing).**
   Spec the interface; bast confirms + implements post-SEC-1. **The verb:**
   `bus recover <agent>` (one verb the engine invokes), which:
   - kills the wedged `claude` PID for `<agent>`,
   - `zellij action close-pane --pane-id <agent's terminal_N>`,
   - `agent-launch <agent>` (respawns, resuming the session by stable UUID),
   - **bumps a per-agent recovery epoch** so in-flight-but-unacked records
     RE-DELIVER (else mail in flight at relaunch is silently lost). This extends
     the existing boot-epoch fence to a per-agent dimension — design note for
     when Phase C wires it: the engine signals the broker to re-arm delivery for
     `<agent>` after a successful recover, treating pre-recover in-flight as
     re-deliverable rather than acked.
   Prior on the contract is the kilvin suspend-wedge recovery pattern.
2. **R6 context-100% — OBSERVE-ONLY until P5.** It's a correctness failure
   (agent alive, lying); only P5's output-verification detects it. Kept DISTINCT
   from P3's fill-%@60% soft-nudge (that's the context-watchdog, not R6).
3. **Δtokens/Δt — APPROVED, but CONSUME kvothe's, don't recompute.** kvothe's
   token-monitor produces it; sync location/format first. Phase A uses
   transcript staleness as the stand-in second signal.
4. **Phase A observe-only ships behind a flag.** It acts nothing (logs
   `would-recover …` only) = zero blast radius. Real actions stay behind
   `CLAUDE_BUS_AUTO_RECOVERY` + the phase gate until B/C. Prove detection on the
   real stream first.

## 12. Phase A — what actually lands now (observe-only)

The minimal, zero-blast-radius slice landing on `main`:
- A new `maybeAutoRecover()` step in the delivery tick (rate-limited ~30 s),
  running **alongside** the existing `maybeAutoClear` (which keeps acting on R1
  — Phase A does NOT touch it; the folding-into-R1 happens in Phase B).
- A lightweight **rule table** (name + would-be action + predicate) covering the
  signatures whose signals exist today: **R2 relaunch-idle**, **R3 hung-turn**,
  **R4 wedged**, plus **R1 idle-context** for completeness. R5 (thinking-block —
  needs an error-signature source, TBD) and R6 (needs P5) are documented
  placeholders, not yet evaluated.
- On a match, append `would-recover agent=… signature=… action=… signals=…` to
  the `audit` topic (protocol `would-recover`), with a per-(agent,signature)
  cooldown so a persistently-wedged agent logs once, not every scan.
- **No ledger, no guards-that-gate-actions, no actions** — those arrive with
  Phase B/C. The 2-signal requirement for would-be-relaunch rows lives in the
  predicate (so the log is honest about what WOULD fire).
- Gated by `CLAUDE_BUS_AUTO_RECOVERY` (default `observe`; `off` disables the
  engine). Caveat carried forward: Phase A reads wall-clock-derived event ages,
  so it WILL emit suspend/resume false positives — that's part of the
  shakedown, and it's exactly why Phase B/C must add the monotonic-clock gate
  before any action fires.
