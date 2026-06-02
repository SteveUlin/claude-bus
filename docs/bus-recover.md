# `bus recover <agent>` — the relaunch primitive (P2 Phase C dependency)

**Status:** CONTRACT ALIGNED with elodin (2026-06-02, §9) — impl follows once his
P2 Phase B firms. **Owner:** bast (pane lifecycle).
**Caller:** elodin's broker auto-recovery engine — the `relaunch` action (R4 /
R3-escalate / R5) *invokes* this verb; it does not reimplement it
([broker-auto-recovery.md](broker-auto-recovery.md) §4 rung 3, §2 non-goal "the
broker does NOT own the relaunch primitive").

## 1. What it is

`bus recover <agent>` automates the **gap-#6 manual recovery** for a wedged
agent, as one idempotent primitive:

1. **Resolve** the agent's live `claude` process + its pane.
2. **Kill** the wedged `claude` (SIGTERM → grace → SIGKILL).
3. **Relaunch** it via `agent-launch <agent>` — which **resumes the prior
   session with `claude --continue`** (NOT a UUID; per the corrected
   resume mechanism — `agent-launch` picks the most recent session in the
   agent's per-workspace cwd). Conversation/in-flight is gone; the session
   resumes where it left off.
4. **Verify** it comes up (fresh process + pane present + events resume).
5. **Report** the outcome (exit code + structured stdout) for the caller.

It is the **single hard rung** of the recovery ladder. The softer rungs (nudge,
`/clear`) are the broker's own (`deliverInline` / `commands-<agent>`); `recover`
is the one that needs bast's pane-lifecycle primitive.

## 2. Boundary — what `recover` does and does NOT do

The clean seam (mirrors elodin's: broker owns policy, bast owns the primitive):

| `recover` (bast) DOES | `recover` does NOT (broker owns) |
|---|---|
| kill the wedged PID | decide *whether* to recover (the signature) |
| relaunch via agent-launch `--continue` | the 2-signal-agree rule |
| verify the agent came up | the MaxR/MaxT circuit breaker |
| report outcome (exit + JSON) | exp-backoff / thrash accounting |
| be idempotent + safe to call | the kill-switch / per-action gate |

**The broker checks its guard BEFORE calling `recover`, and records its ledger
AFTER reading the outcome** (elodin §3: `guard.allows` → `action` →
`ledger.record`). So `recover` is **stateless w.r.t. recovery policy** — it never
reads the breaker, never refuses on backoff. If the broker called it, the broker
already decided it's allowed. This keeps the breaker logic in one place (the
engine) and the primitive dumb + testable.

## 3. The contract (what the broker can rely on)

```
bus recover <agent> [--no-verify] [--timeout-ms N]
```

**Exit codes** (the breaker's primary signal — semantics settled with elodin;
**`0` AND `10` both COUNT toward MaxR**, because a relaunch *happened* = a
disruption, and repeated "successful" relaunches of a flapper IS the loop the
breaker bounds):
- `0` — recovered **and verified** (fresh claude up, a fresh event landed). Engine:
  record success, `count++`, clear the active signature.
- `10` — relaunched but **verify timed out** (process started, no fresh event yet —
  possibly **boot-stuck**). Engine: `count++`, leave the agent `verifying`,
  re-check next scan (soft). The boot-stuck-vs-healthy distinction is exactly why
  verify is event-based (§6).
- `20` — **failed** (couldn't resolve/kill, or relaunch never started). Engine:
  escalate-human (`inbox-ops`) + **open the breaker now**.
- `30` — **attached-pane defer** (human is on the pane; `recover` did nothing).
  Engine: **no count**, defer, re-eval later.
- `2` — usage / unknown agent.

**stdout** — one JSON line (the audit + the detail the ledger may fold in):
```json
{"agent":"kvothe","action":"recover","killed_pid":12345,
 "relaunched":true,"verified":true,"elapsed_ms":4200,
 "session_resumed":true,"error":null}
```

`recover` is **idempotent**: if no live `claude` is found (already dead), it skips
the kill and proceeds to relaunch — a recover on an already-down agent still
brings it up. Two concurrent `recover <agent>` calls are serialized by a per-agent
flock (`$STATE/recovery/<agent>.lock`) so a double-fire can't double-launch.

## 4. Ledger ownership — RESOLVED (elodin, 2026-06-02)

**The verb writes NOTHING to `$STATE/recovery/<agent>.json`.** That file is
breaker POLICY (MaxR/MaxT window + backoff) and is **single-writer = elodin's
engine** — two writers is literally the "crash-loop bounces the broker → infinite
relaunches" bug (elodin §7). `recover` returns its outcome via **exit code + the
one JSON stdout line** (§3); the engine's `ledger.record(agent, row)` folds it in.
No verb-side `attempts.jsonl` — elodin's engine already audits every action to the
audit topic, so a verb-side trail is redundant (revisit later only if we want raw
verb telemetry distinct from policy). **Breaker single-writer, full stop.**

## 5. The relaunch mechanic — respawn-loop pane (CHOSEN, elodin-confirmed)

The wedged pane runs `… exec agent-launch <agent>` (fleet.kdl), so when `claude`
dies the pane's command is *done* — no shell to re-run agent-launch, so "kill"
alone doesn't relaunch. **Fix: make the fleet.kdl agent panes a guarded
respawn-loop** (the pattern `monitor`/`agent-bar`/`bus-deck` already use):

```sh
while :; do
  agent-launch <agent>
  [ -e "$STATE/recovery/<agent>.down" ] && break   # the down-sentinel (§5.1)
  sleep 2
done
```

Then **`recover` is just "kill the PID"** — the pane's own loop relaunches (fresh
`claude --continue`). The relaunch is local to the pane (no cross-pane zellij
driving), self-heals on *any* claude death (not only recover-triggered), and
reuses a proven pattern. Cost: a one-line fleet.kdl change per agent (mine) + the
sentinel. elodin confirmed the **double-trip is bounded by timescale** — his
wedge/recovery detection needs *sustained* signal (`WEDGE_BUDGET_MS` = minutes)
while the respawn window is ~2 s, so a natural fast respawn never trips him; only a
*persistent* boot-stuck loop engages both, and his OTP breaker (3/10 min) bounds
that. He also confirmed his `SessionEnd` handler (delivery.cpp:375) releases
in-flight **without advancing the cursor**, so a respawn re-delivers to the fresh
TUI cleanly.

(Rejected: verb-driven re-exec via a zellij action — fragile TTY contention,
`close_on_exit` dependence, and a different path for fleet-fixed-tab vs dynamic
peers. Dynamic peers' P4 respawn spec exists but despawn+spawn changes tab
identity. Kill-only + loop is strictly cleaner.)

### 5.1 The down-sentinel — `$STATE/recovery/<agent>.down` (the breaker↔loop seam)

The sentinel is a **contract item**, the seam between elodin's breaker and bast's
loop:

- **Writers (two):** (1) elodin's engine, when the **breaker OPENS** (gave up after
  MaxR) — "the broker gave up, stop respawning"; (2) an **intentional despawn/quit**
  (so a deliberate shutdown doesn't auto-respawn).
- **Reader:** the pane loop, checked **before each respawn** — present ⇒ `break`,
  the pane stays down, the human intervenes.
- **Cleared by `agent-launch` at startup:** a fresh *intended* launch removes the
  sentinel (a launch means "intended up"), so a human relaunch/spawn after a
  breaker-open clears the halt and the loop resumes.
- Path: **`$STATE/recovery/<agent>.down`** (bast's pick) — co-located with elodin's
  recovery state, clearly recovery-owned.

elodin owns two engine-side pieces that complete the seam: **(a)** on a *verified*
fresh `SessionStart` he resets the agent's recovery state (active sig +
per-signature backoff) so stale alarms don't re-trip (ties to his `forgetAgent`
eviction); **(b)** he writes the sentinel on breaker-open. bast owns the loop +
the `agent-launch`-clears-on-start behavior.

## 6. PID resolution + verify (mechanics)

- **Find the PID:** `agent-launch` runs `claude --name <agent>` (unique), so
  `pgrep -f "claude --name <agent>"` resolves it. **Better (deterministic, my
  lane):** have `agent-launch` write `$STATE/pids/<agent>` at launch and `recover`
  read it — no pgrep heuristic, no ambiguity with a stale match. Proposing to add
  the pidfile to agent-launch as part of this work.
- **Kill:** SIGTERM, wait `grace` (default 3 s), SIGKILL if still alive. (claude
  may ignore SIGTERM mid-stream — SIGKILL is the backstop, same as `pane.cpp`'s
  `waitWithTimeoutOrKill`.)
- **Verify — EVENT-BASED by default (elodin's call, stronger).** After relaunch,
  **block until a fresh event for the agent lands in `events.jsonl`** (`ts` after
  the relaunch — `SessionStart` / first hook), up to `--timeout-ms` (default
  **10–15 s** for boot headroom). Process-up + pane-present is **not** enough: it
  can't tell a healthy resume from a **boot-stuck** relaunch (came up, hung on
  boot) — and a weak verify would falsely report `0` and *mask a loop*. Event-verify
  catches boot-stuck → returns `10` → the engine re-checks → the breaker eventually
  opens. That distinction is exactly the signal the breaker needs. `--no-verify`
  (skip, return as soon as the process is up) and `--timeout-ms` are for tests /
  callers that re-check on their own scan.

## 7. Safety (the verb's own, beyond the broker's guard)

- **Per-agent flock** (`$STATE/recovery/<agent>.lock`) — no double-launch from a
  concurrent fire.
- **Attached-pane defer:** if the human is attached (`hasPresenceFile(agent)`),
  `recover` refuses with a distinct exit (`30`) — the human has the pane; don't
  kill under them. (Mirrors the broker's `hasPresenceFile` defer; belt-and-
  suspenders since the broker also checks it.)
- **Never the broker's own pane:** `recover broker` / the ops pane is out of
  scope — the verb only targets agent panes it can resolve a session for.

## 8. Test plan (isolated `$STATE`, fake-zellij — the existing itest harness)

- **kill+relaunch:** a fake `claude --name X` process + fake-zellij pane; `recover
  X` kills it and (mechanic A) the loop / (mechanic B) the verb brings a fresh one
  up; assert new PID, exit `0`, JSON `verified:true`.
- **idempotent on already-down:** no live PID → still relaunches, exit `0`.
- **verify-timeout:** relaunch that never comes up → exit `10`, `verified:false`.
- **attached defer:** presence file present → exit `30`, no kill.
- **concurrent fire:** two `recover X` → flock serializes, exactly one launch.

## 9. Alignment — ALL RESOLVED with elodin (2026-06-02)

1. **Ledger ownership** (§4) — RESOLVED: verb writes nothing; returns exit+JSON;
   breaker state single-writer (engine); no verb-side `attempts.jsonl`.
2. **Exit codes** (§3) — RESOLVED: `0` & `10` both count toward MaxR; `20` opens
   breaker + escalates; `30` no-count defer. Maps onto his escalate-vs-retry.
3. **Verify depth** (§6) — RESOLVED: event-based by default (catches boot-stuck →
   `10`); `--no-verify` / `--timeout-ms` (~10–15 s) for tests.
4. **Respawn-loop mechanic** (§5) — RESOLVED: double-trip bounded by timescale
   (his `WEDGE_BUDGET_MS` = minutes ≫ ~2 s respawn); his `SessionEnd` re-delivers
   cleanly (delivery.cpp:375). The **down-sentinel** (`$STATE/recovery/<agent>.down`)
   is the breaker↔loop seam (§5.1) — engine writes on breaker-open, loop halts.

**Build split (when elodin's Phase B firms):** bast — the `bus recover` verb (kill +
relaunch + event-verify + exit/JSON), the fleet.kdl respawn-loop, the
`agent-launch` pidfile + sentinel-clear-on-start, the itest (§8). elodin — engine
calls the verb behind its guard, records the ledger from the outcome, writes the
down-sentinel on breaker-open, resets recovery state on verified `SessionStart`.
