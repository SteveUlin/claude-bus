# `bus recover <agent>` — the relaunch primitive (P2 Phase C dependency)

**Status:** DESIGN — contract first (the deliverable); impl follows once elodin's
P2 Phase B firms and we align the interface. **Owner:** bast (pane lifecycle).
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

**Exit codes** (the breaker's primary signal):
- `0` — recovered **and verified** (fresh claude up, events resuming).
- `10` — relaunched but **verify timed out** (process started, not yet confirmed
  live). Caller may treat as soft-fail: re-check next scan rather than escalate.
- `20` — **failed** (couldn't resolve/kill, or relaunch never started). Caller
  escalates-human + opens the breaker.
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

## 4. THE ALIGNMENT QUESTION (elodin) — ledger ownership

auri's brief says "the verb must read/write the same `$STATE/recovery/<agent>.json`
shape." elodin's §7 says that ledger holds **breaker state + relaunch timestamps +
per-signature backoff** — i.e. **recovery POLICY state, which the engine owns**.
Two models; I lean A, but it's elodin's call since his breaker reads it:

- **(A) RECOMMENDED — verb returns, broker records.** `recover` writes **nothing**
  to `recovery/<agent>.json`; it returns outcome via exit+stdout, and the broker's
  `ledger.record(agent, row)` folds it in (timestamp on success, breaker-trip on
  `20`). Cleanest: one writer (the engine) owns the breaker file; the primitive
  stays stateless. Matches §3 exactly.
- **(B) Verb appends an attempt-record.** `recover` appends a `{ts, outcome,
  killed_pid}` event to a *separate* `recovery/<agent>.attempts.jsonl` (audit
  trail), which the engine reads alongside its own ledger. Keeps the breaker file
  engine-owned but gives a verb-authored audit log.

**I do NOT want the verb writing the breaker file (A's rejection of that) — two
writers to the MaxR/MaxT state is the bug that gives "a crash-loop bounced the
broker → infinite relaunches" (elodin §7's exact warning).** Proposing A; B if
elodin wants the verb-side audit trail. Either way, **the breaker state stays
single-writer (engine).** Need elodin's pick before I build.

## 5. The relaunch mechanic (bast's lane) — two candidates

The wedged pane runs `… exec agent-launch <agent>` (fleet.kdl), so when `claude`
dies the pane's command is *done* — there's no shell left to re-run agent-launch.
So "kill" alone doesn't relaunch. Two ways to close that:

- **(A) RECOMMENDED — respawn-loop pane.** Change the fleet.kdl agent panes from
  `exec agent-launch <agent>` to a **guarded loop** —
  `while :; do agent-launch <agent>; [ -e $STATE/down/<agent> ] && break; sleep 2; done` —
  exactly the pattern the viewers (`monitor`, `agent-bar`, `bus-deck`) already use.
  Then **`recover` just kills the PID**; the pane's own loop relaunches (fresh
  `claude --continue`). A `$STATE/down/<agent>` sentinel breaks the loop for an
  *intentional* despawn/exit (so a deliberate quit doesn't auto-respawn). Pros:
  the relaunch is local to the pane (no cross-pane zellij driving), survives any
  claude crash (not just recover-triggered), reuses a proven pattern. Cons: a
  one-line fleet.kdl change per agent + the sentinel; changes exit semantics
  (intentional exits must touch the sentinel).
- **(B) Verb-driven re-exec.** Keep `exec`; `recover` kills the PID, then drives
  the pane to re-run agent-launch via a zellij action (needs `close_on_exit=false`
  so a shell remains, then inject the command). Fragile: TTY contention, depends on
  zellij pane-command semantics, different path for fleet-fixed-tab vs dynamic
  peers. For **dynamic peers** the P4 respawn spec (`$STATE/dynamic-peers`,
  [P4](broker-auto-recovery.md)) already records role+project_dir → a despawn+spawn
  is possible, but that changes tab identity.

**Recommend A** (respawn-loop): it makes `recover` trivially reliable (kill-only)
and the pane self-heals on *any* claude death. The fleet.kdl change is mine.

## 6. PID resolution + verify (mechanics)

- **Find the PID:** `agent-launch` runs `claude --name <agent>` (unique), so
  `pgrep -f "claude --name <agent>"` resolves it. **Better (deterministic, my
  lane):** have `agent-launch` write `$STATE/pids/<agent>` at launch and `recover`
  read it — no pgrep heuristic, no ambiguity with a stale match. Proposing to add
  the pidfile to agent-launch as part of this work.
- **Kill:** SIGTERM, wait `grace` (default 3 s), SIGKILL if still alive. (claude
  may ignore SIGTERM mid-stream — SIGKILL is the backstop, same as `pane.cpp`'s
  `waitWithTimeoutOrKill`.)
- **Verify:** after relaunch, within `--timeout-ms`, confirm (a) a NEW
  `claude --name <agent>` PID exists (different from the killed one), (b) its pane
  is present (`bus pane-id <agent>` resolves), and ideally (c) a fresh event for
  the agent appears in `events.jsonl` with `ts` after the relaunch (SessionStart /
  first hook). `--no-verify` skips (c) for callers that re-check on their own scan.

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

## 9. Open items for elodin (align before building)

1. **Ledger ownership** (§4) — A (return-only, recommended) vs B (verb-side audit
   log). Breaker state stays single-writer (engine) either way.
2. **Exit-code contract** (§3) — does `10` (relaunched-unverified) vs `20`
   (failed) map cleanly onto his breaker's escalate-vs-retry decision? Tune the
   codes to what his engine needs.
3. **Verify depth** — is process+pane enough, or does his engine want `recover` to
   block until an *event* resumes (stronger, slower)? `--no-verify` / `--timeout`
   give him the knob.
4. **Mechanic A's fleet.kdl change** — does the respawn-loop interact with anything
   broker-side (e.g. the post-relaunch `SessionStart` handling at delivery.cpp:366)?
   Confirm the auto-relaunch doesn't double-trip his recovery signatures.
