# Broker Triggers — event→script, broker-side, async

Status: **design / proposal** (2026-06-09). Feature-3 of sulin's three: *"make
Claude Code hooks into events that get passed into the broker; the broker
launches scripts based on these events."* The fact-extraction hook is its first
use.

## 1. Problem & thesis

Today an agent's per-turn reactions are **agent-side hooks**: `settings/
hooks-shared/*.sh` run *inside* the agent's process on `Stop`/`PostToolUse`/etc.
That's right for things the turn genuinely needs (drain its inbox, write its
readiness sentinel, render its statusline). It's the **wrong place** for a
cross-cutting, off-turn reaction — say, *"after every turn, extract durable
facts with a cheap model"*:

- It **blocks the turn.** A `Stop` hook that runs `claude -p` stalls the agent
  until the extraction finishes, and burns the agent's own rate limit.
- It forces **per-agent config drift.** Only *some* agents want it, so you'd
  have to vary `settings.json` per agent — against the "one ruleset" rule.

**Thesis: the agent only EMITS events; the broker REACTS.** Agents already write
a structured event stream (`events.jsonl`, via `log-event.sh`) that the broker
already tails (`scanEvents`). Add one thing: a **trigger table** the broker owns
— *event pattern → script* — and run the matched script **async, broker-side,
off the agent's turn.** Per-agent/per-role variation lives in **one landed
table**, not in N agents' settings. This is the configurable, script-capable
expression of the Policy substrate ([[policy-actors]] / the "retire the
hooks+scripts layer" §9 direction): a new **`RunScript`** action beside the
compiled `Enqueue`/`Recover`/`Nudge` actors.

Triggers **complement, don't replace** the agent-side per-turn hooks. Drain /
statusline / ready-write stay where they are (the turn needs them). Triggers are
for reactions the turn does *not* need to wait on.

## 2. The model

```
  agent hook ──append──> events.jsonl ──tail──> broker ──match──> trigger table
                                                                │
                                                   RunScript action (async)
                                                                │
                                          fork-detached script  ← event JSON on stdin
```

- **Event in** — unchanged. Hooks append to `events.jsonl`; the broker already
  reads new lines each tick (`scanEvents`, byte-offset cursor). No new IPC, no
  hook→broker socket call (durable + replayable + survives a broker restart —
  the same reason the off-TTY drain reads a file, not a push).
- **Match** — each new event is matched against the trigger table (event name +
  agent/role + payload conditions).
- **RunScript out** — a match emits a `RunScript` policy action.
- **Execute** — the loop **fork-detaches** the script (never waits) and passes
  the event JSON on the script's stdin, exactly as Claude Code passes a hook its
  payload. A trigger script *is* a hook script — just run by the broker, async.

## 3. The trigger table

Landed from `main` and materialized like the hooks (`settings/triggers.kdl` or
`.json`); the broker loads it at startup (config propagates by land + broker
restart, like everything). One entry:

```kdl
trigger "fact-extract" {
    on "Stop"                       // event name (root .event)
    match role="scholar"            // optional: role / agent / payload.* conditions
    run  "settings/triggers/fact-extract.sh"
    model "haiku"                   // convenience env for the script
    cooldown_ms 0
    max_concurrent 2                // backpressure (see §5)
}
```

The script receives the event JSON on stdin (root `{ts, session, agent, pane,
event}` + `payload.{transcript_path, tool_name, source, …}`) and `$CLAUDE_BUS_*`
env. It does its own work and exits; the broker does not read its output.

## 4. Integration with the Policy substrate (grounded in the code)

Reuse the existing "loop folds observations into a snapshot → actor decides →
loop executes" pattern — no new plane:

- **`PolicyContext` gains `std::vector<Event> new_events`** (the events read this
  tick), folded by the loop plane exactly as it already folds `queue_head` /
  `board_updates` (`policy.h` §1.3 "new observation needs are new fields").
- **A `TriggerActor`** (new `PolicyActor`, lives in `bus_policy`) holds the
  loaded table; `evaluate(ctx)` matches `ctx.new_events` against it and returns
  `RunScript` actions. Pure + reactive — fits the actor interface as-is.
- **`PolicyAction::Kind` gains `RunScript`** (today: `Enqueue`/`Recover`/
  `Nudge`), with fields for the script path + resolved args. `executePolicyAction`
  (`delivery.cpp:1276`) gains a `case RunScript:` that fork-detaches.

The kernel (cursor/ack/in-flight) is **untouched** — a trigger never advances a
delivery cursor or moves mail; it only spawns a side-effecting script.

## 5. Async execution — the load-bearing constraint

The broker is **single-threaded** (the delivery/pselect loop). It **must never
block** on a triggered script — a `claude -p` takes seconds; blocking the loop is
the exact saturation-wedge the pane-fork discipline exists to prevent. So:

- **Fire-and-forget spawn.** `process.{h,cpp}` (the existing spawn module) is
  *blocking-with-timeout* — wrong for this. Triggers need a new
  **`spawnDetached(argv, env, stdin_bytes)`** primitive: `fork`, in the child
  `setsid` + redirect stdio (event JSON → stdin, stdout/stderr → a per-trigger
  log or `/dev/null`), `exec`; the parent returns immediately and **does not
  wait**.
- **Reaping without blocking.** A detached child re-parents to init on exit
  (double-fork), so the broker needn't `waitpid` — no zombies, no SIGCHLD
  handling in the hot loop.
- **Backpressure (mandatory).** A trigger that fires every `Stop` across a busy
  fleet could spawn dozens of `claude -p` at once — cost + rate-limit blowout.
  The broker caps concurrent trigger processes (per-trigger `max_concurrent` +
  a global ceiling); over the cap, **drop with an audit event** (never queue
  unboundedly, never block). Triggers are best-effort by construction.
- **Best-effort, not at-least-once.** Unlike mail, a missed trigger is not
  re-delivered (it has no cursor). A trigger that must not be lost should write
  durable state itself (the fact-extract script appends to a file). State this
  so no one mistakes a trigger for guaranteed delivery.

## 6. First trigger: fact-extraction → the facts log

The motivating use, and it *is* the record-facts / P3 distiller policy
([[project_facts_log_learnings_model]]):

```
on Stop(agent) where role wants it
  → fact-extract.sh  (event JSON on stdin)
      reads payload.transcript_path
      runs: claude -p --model haiku "extract durable, reusable facts from this turn"
      appends the facts to $STATE/facts/<agent>.jsonl   (write-only log)
```

Broker-side wins decisively here: it runs **beside** the agent's next turn (no
block, no contention for the agent's rate limit), on a **cheap model**, and the
"which agents do this" lives in one trigger entry. The recurrence-gated
distillation step of the facts-log model is then itself a second trigger (or a
periodic policy) over the accumulated `$STATE/facts/*`.

## 7. Invariants & safety

- **Never block the loop** (§5) — the one hard rule.
- **Kernel-untouched** — triggers don't advance cursors / move mail / change
  delivery; a trigger crash can't corrupt at-least-once.
- **Landed-from-main only** — the broker launches scripts; the table + scripts
  materialize from `main` exactly like hooks (no live-editable trigger surface,
  no per-agent injection). A trigger is as trusted as a hook, no more.
- **SEC-1 interaction** — a trigger script that hits the network (a `claude -p`
  does) must honor the same netns/squid cage as the agent it fires for, or run
  uncaged by explicit policy. Flag at build.
- **Observability** — every fire (and every backpressure drop) emits an audit
  event, so `bus log` shows the trigger history.

## 8. Migration — smallest first slice

1. **`spawnDetached` primitive** + a unit test (spawn `/bin/sh -c 'echo $X'`,
   confirm fire-and-forget + stdin delivery + no broker block).
2. **`RunScript` action + `TriggerActor` + `new_events` in `PolicyContext`** with
   a **hardcoded** single trigger (fact-extract on `Stop`), behind an env flag —
   prove the path end-to-end on one agent with no config surface yet.
3. **The configurable table** (`settings/triggers.*` + loader + `match`
   conditions + backpressure caps). Now triggers are data, not code.
4. **Fact-extract script + the facts log**, then the distillation trigger.

Each step keeps the broker delivering mail; the kernel never changes.

## 9. Open questions for sulin

- **Table format** — KDL (matches layouts) vs JSON (matches settings)? Lean KDL.
- **Match expressiveness** — start with `event + role/agent + simple payload
  equality`; defer a full predicate DSL (the §9 "declarative policy DSL" is the
  horizon, not v1).
- **Per-trigger vs global concurrency cap** defaults, and the drop-vs-defer
  policy at the cap (proposal: drop + audit; never defer).
- **Caged or uncaged** for network-hitting trigger scripts (SEC-1 tier).
- **`spawnDetached` location** — extend `process.h`, or a sibling `spawn.h` (it's
  a different contract — no return value, no wait). Lean sibling.

## 10. Out of scope

- **No hook→broker socket/RPC** — events flow through the durable `events.jsonl`
  tail, full stop.
- **No migration of the existing per-turn hooks** (drain/statusline/ready-write
  stay agent-side) in v1 — the long-term "retire the hooks layer" direction
  rides on this substrate but is not this build.
- **No reading a trigger's output back into the broker** — fire-and-forget; a
  trigger that needs to feed the bus does so by writing state / enqueuing mail
  itself.
- **No per-agent live trigger config** — landed-from-main only, per-role via the
  `match`.
- **The full agent-as-policies composition** ([[project_policy_coordination_substrate]])
  — triggers are one step toward it, not it.
