# M2 — the work-queue as a Policy actor (the unification MVP)

Status: **design + implementation plan** (elodin, 2026-06-05), for sulin's
review. **Gate: sulin approves this doc → build begins. No build before
sign-off** — same discipline as C1. Owner: elodin. Reference:
[[policy-actors]] §6/§6.1 (the unification milestone) and §0.1 (the north star).
Task #1780675724650-elodin-1214.

## 0. What M2 proves

C1 landed the Policy substrate and proved it with one *broker-internal* actor
(recovery). M2 proves the substrate is **general** — that a *coordination
pattern*, which looks nothing like recovery, drops onto the same `PolicyActor`
interface and needs **no bespoke loader and no per-agent hooks**. One lean
pattern, end-to-end. MVP over breadth (sulin's steer): the milestone is the
*framework result* — "a coordination-shaped actor runs on the same engine" —
not the queue's feature set.

**The deliverable insight** (answering `coordination/README`'s open *"how does a
pattern activate"*): **a coordination pattern's broker-side IS a Policy actor.**
Activation = the work-queue topic exists + the `DispatchActor` is already
registered. No loader, no agent-prompt teaching idle-fetch behavior, no per-agent
Stop-hook pulling the next item. The broker assigns centrally; agents receive
work as ordinary drained inbox mail with zero coordination code.

## 1. Why work-queue (the MVP-minimal pick)

Of the coordination patterns, the work-queue needs the least new code because
**the broker already owns most of it**:

| concept | already built |
|---|---|
| the queue | a `work-queue` topic = an append-Log (`kKindWorkQueue`) |
| produce | `bus msg enqueue work-queue <task>` |
| claim/consume | the `fetch` RPC: reads the head at the shared `""` cursor, atomically advances past it (`broker.cpp` fetch handler) |
| readiness | `computeAxes(...).turn == Ready` — the snapshot C1 already builds |
| delivery to the agent | the agent-inbox topic + the off-TTY drain (agents already pull their inbox via the drain hook) |

The only missing piece is the **dispatch decision** — "this idle agent should
get the next task" — which is precisely a Policy actor's job. So M2's new code is
essentially *just the `DispatchActor`* plus a snapshot field.

**Today's model is a *pull*:** `work-queue` consumers run `bus msg fetch` and the
fetch advances the shared cursor (broker-spec §"Topic kinds"). That requires each
agent to actively fetch on idle — the distributed, racy, per-agent-hook model
`coordination/README` describes. M2 inverts it to a **central push**: one
`DispatchActor` watches queue-depth × idle-agents and assigns.

## 2. The core tension — claim vs. the kernel guard

A work-queue must **claim** each task (consume it so it isn't re-assigned). In
the kernel that claim is a **cursor advance** (the `fetch` RPC advances the
shared `""` cursor). But §1.4 of [[policy-actors]] is explicit: **a Policy actor
has no cursor verb — it cannot advance a cursor or ack a record.** That guard
exists to protect the agent-inbox *at-least-once* invariant from a buggy actor.

So M2 genuinely tests the vocabulary: **can a competing-consumer pattern be
expressed without breaking the no-cursor-verb guard?** Three resolutions, each
real:

- **(A) consume-and-route — RECOMMENDED.** The actor emits an `Enqueue` carrying
  a `consume_from` field (the source work-queue topic). The **loop executor**
  (kernel plane, not the actor) performs the claim: fetch the work-queue head
  (advance the `""` cursor, the *existing* fetch primitive) and deliver the task
  body to `inbox-<agent>`. **The actor never advances a cursor — the loop does**,
  on the actor's declarative consume-intent, exactly as the `fetch` RPC advances
  on an agent's pull. §1.4 holds literally (the *actor* touches no cursor); the
  kernel triad is untouched (the work-queue cursor advances on *consume*, its
  existing pull semantics — NOT an agent-inbox at-least-once cursor, which still
  advances only on the recipient's ack). No new action **kind** — a field on
  `Enqueue`. This is the §2-foreseen *localized* vocabulary extension, and it
  reuses `fetch` rather than adding a claim mechanism.

- **(B) actor-owned claim set — the even-purer alternative.** The actor keeps a
  persisted `assigned` set of task-ids (like the recovery ledger), reads the
  queue window from the snapshot, skips already-assigned tasks, and emits plain
  `Enqueue{inbox-<agent>, task-body}`. **Zero executor change, zero cursor
  touch, no new field** — pure §1.4. Cost: the actor reimplements the claim in
  its own state and the work-queue's cursor goes unused (the queue is a pure
  append-log, retention-trimmed); correct only while the `DispatchActor` is the
  *sole* consumer (no competing fetchers). Leanest if we accept actor-owned
  claim state.

- **(C) nudge-to-fetch — rejected for the MVP.** The actor `Enqueue`s "work
  available" to an idle agent, which then runs `bus msg fetch` (kernel advances
  the cursor). Pure vocabulary, but it keeps a **per-agent fetch behavior** —
  weakening the very "no per-agent hooks" insight M2 exists to prove. Listed for
  completeness; not recommended.

**Recommendation: (A).** It is fully central (agents need zero coordination
code — they receive ordinary drained mail), it reuses the existing `fetch`
claim, and it keeps the guard intact by putting the cursor advance in the loop
(where `onAck` authority already lives), not the actor. (B) is the fallback if
sulin prefers zero executor change and accepts actor-owned claim state. The pick
between (A) and (B) is the **one real decision** in this doc.

## 3. The `DispatchActor` on the `PolicyActor` interface

```
class DispatchActor : public policy::PolicyActor {
  // name() = "dispatch"
  // evaluate(ctx):
  //   - read the queued tasks from the snapshot (§3.1) and the idle agents
  //     (ctx.agents where axes.turn == Ready, !attached, !blocking_op,
  //     !has_in_flight — the same readiness the doorbell + recovery use)
  //   - pair the head task(s) with idle agent(s), FIFO
  //   - emit, per pairing, an Enqueue{ inbox-<agent>, body = task,
  //       consume_from = "work-queue" }   (resolution A)
  //   - own cooldown: at most one assignment per agent per tick; a small
  //     scan rate-limit, like recovery's 30 s cadence
};
```

It maps cleanly: `evaluate` reads a snapshot, applies its own guard (one
assignment per idle agent per tick), and returns `Enqueue` actions. **No new
action kind** (A adds a field; B adds none). **No kernel touch** (the loop reuses
`fetch`). This is the §2/§2.1 extension argument exercised for real:
coordination = an actor that watches state and posts records.

### 3.1 What the snapshot must add

The actor is pure (reads only `ctx`), so the loop folds the queue head into the
context. A generic field keeps `PolicyContext` pattern-agnostic:

```
// PolicyContext gains:
std::vector<QueuedTask> queue_head;   // {topic, id, body} — head window of the
                                      // work-queue(s), peeked by the loop plane
```

The loop peeks the `work-queue` topic from its `""` cursor (a bounded window =
the number of idle agents, so the cost is O(idle), not O(queue)) and folds the
head tasks in. Future coordination patterns add their own topic to the same
field — the context stays generic, exactly as §1.3 prescribes ("new observation
needs are new snapshot fields").

### 3.2 MVP simplifications (deliberate, noted)

- **Batch-assign** (v2, landed): one task per eligible idle agent per tick, in
  FIFO head order — the loop's in-order consume gives task[i] → agent[i], so the
  multi-consume ordering is safe (single-threaded execution). The first cut was
  one-per-tick; batch drains a burst across the idle fleet in a single tick.
  Load-balancing / rebalance is still a follow-on.
- **Assign-and-forget**: the assigned-set / cursor prevents re-assignment; what
  happens if an agent dies mid-task (re-queue) is a v2 concern, not the MVP.
- **Sole consumer**: the `DispatchActor` is the only work-queue consumer in the
  MVP (no concurrent `bus msg fetch` by agents), so there's no competing-consumer
  race to arbitrate — consistent with §2.1 (the engine never arbitrates).

## 4. How a pattern activates (the deliverable, made concrete)

`coordination/README` leaves *"how does a pattern activate"* unresolved. M2's
answer, demonstrated:

1. Create the topic: `bus topic create work-queue --kind work-queue`.
2. The `DispatchActor` is **already registered** (in `load()`, beside the
   recovery actor) — it was inert because no work-queue existed (empty
   `queue_head` → no actions). Now it assigns.
3. Producers `bus msg enqueue work-queue <task>`; idle agents receive tasks as
   ordinary drained inbox mail.

**No loader, no `coordination/queue/hooks/`, no agent-prompt edit.** A
`coordination/queue/README.md` documents the pattern as "register the topic; the
broker's DispatchActor does the rest" — the pattern is *broker-side code that
already ships*, activated by a topic. That is the unification.

## 5. Build sequence (incremental, broker-live each commit)

- **M2.1 — `DispatchActor` + snapshot field + registration** *(pure add; inert
  until a work-queue topic exists)*. New `dispatch_actor.{h,cpp}` in `bus_policy`
  (links bus_readers only — the §5.4 link guard, nm-verified). Loop folds
  `queue_head` into `PolicyContext`; `load()` registers the actor. For
  resolution (A): add the `consume_from` field to `PolicyAction` + the executor
  branch in `executePolicyAction` (fetch-head-advance-cursor + Enqueue). **Behavior
  is zero-change on the live fleet** (no work-queue topic → empty `queue_head` →
  no assignments), same inert-by-default safety as C1's observe mode.
  - Tests (the litmus — synthetic context, no broker): `DispatchActor::evaluate`
    with a queue head + idle/busy agents → asserts one `Enqueue` to the idle
    agent, none to busy/attached ones; an already-assigned/empty queue → no
    actions. Executor test (A): a `consume_from` Enqueue advances the work-queue
    cursor exactly once and delivers the body.
- **M2.2 — `coordination/queue/README.md`** *(docs only)*: the activation recipe
  + the "broker-side IS the actor" statement. Closes the README's open question.
- **Deploy-verify** *(folds the C1 live cutover — auri)*: M2 is the first
  behavior-changing follow-on that needs C1's engine running live, so the live
  broker must carry C1 first. Sequence:
  1. Isolated `CLAUDE_BUS_STATE`: launch the M2 binary, `bus topic create
     work-queue`, enqueue N tasks, drive idle fake-agents → assert each task
     lands in exactly one idle agent's inbox, the queue drains, no double-assign.
  2. **Surgical live cutover**: resolve the running broker's specific pid
     (`bus broker info` / pidfile), `SIGTERM` *that* pid (never `pkill -f`),
     floating relaunch (`zellij action new-pane --floating -- <path>/bus broker
     run`, never `nohup`). Confirm `bus broker info` shows the new build_commit.
  3. Live smoke: a mail round-trip still works; the recovery would-recover stream
     is unchanged (C1 still neutral); the work-queue stays empty/inert unless a
     work-queue topic is created — so the live cutover itself changes no behavior
     beyond "the engine + DispatchActor are now live."

## 6. Guards held

- **Kernel triad** survives: append-log (Enqueue appends), agent-inbox
  cursor-advances-on-ack-only (the consume_from advance is the *work-queue* pull
  cursor, never an inbox cursor; inbox delivery still acks normally), boot-epoch
  (the Enqueued task is epoch-stamped like every other record).
- **§1.4 no-cursor-verb**: the *actor* advances no cursor in either (A) or (B);
  in (A) the *loop* performs the work-queue consume (kernel plane), in (B) no
  cursor moves at all.
- **§4 leaf guard**: the `DispatchActor` owns only its own state (cadence; in (B)
  the assigned-set) — no shared mutable cross-actor field; coordination with
  other actors, if ever needed, goes through the Log.
- **§2.1 composition**: the engine still never arbitrates; the work-queue's
  single-consumer MVP means no same-target conflict to resolve.

## 7. Open decision for sulin

**(A) consume-and-route vs. (B) actor-owned claim set.** (A) reuses the kernel's
`fetch` and is fully central with a one-field vocabulary extension; (B) is the
purest (zero executor change) at the cost of actor-owned claim state and a
sole-consumer assumption. I recommend **(A)**. Everything else (the actor shape,
the snapshot fold, the activation story, the build sequence) is identical between
them. One ratify, then build.
