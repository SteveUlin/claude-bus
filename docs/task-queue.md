# A Task Queue, Distinct From Mailbox

A mailbox is a place to receive messages. A task queue is a place to
receive *commitments to do something long-running*. Both can be FIFO,
both can be single-recipient, both can advance on the recipient's
attention. They're not the same concept and sulin wants the
distinction made first-class.

## Concrete examples

1. **CI-like batch.** Three serial jobs on `kvothe`: refactor TC22-27,
   migrate to isolated-broker, commit + push. Each touches files the
   previous touched. Job N+1 must not start until job N is finished
   and committed.
2. **Benchmark series.** Run N benchmark cases against a fresh broker
   for each. Same operator, same machine, parallel is meaningless;
   the queue *is* the experiment plan.
3. **Multi-step refactor.** "Extract types from eval.h → rename Foo →
   update callers → land." A chain that wants to flow at the speed
   the agent works, not at the speed of the producer.
4. **Replay.** A user records a sequence of operations they want
   re-executed against a clean state. Each one must finish before
   the next dispatches.

In all of these, the cursor wants to advance on **task completion**,
not on task receipt. Mailbox ACK fires on `UserPromptSubmit` —
"received and engaged" — which is too early.

## What's distinct, technically

| Property                | inbox-X (mailbox) | tasks-X (proposed) | work-queue |
| ----------------------- | ----------------- | ------------------ | ---------- |
| Recipients              | 1                 | 1                  | N          |
| Default `deliver_when`  | immediate         | idle               | n/a (pull) |
| Cursor advance trigger  | UserPromptSubmit  | UserPromptSubmit + idle-gate on next | fetch |
| Conceptual contents     | messages          | work commitments   | independent items |
| Mixes with mail?        | yes (same topic)  | **no — different topic** | no |

The conceptual point is the column the table doesn't capture: when a
human or peer drops items on `tasks-kvothe`, they're saying *"do these
in order, don't interleave with chatter."*

## Candidate designs

**A. New topic kind `task-queue`.** Broker-side semantics: single
recipient, FIFO, ACK on explicit completion signal (`bus task done
ID`). Pros: clean ACK semantics, monitor-visible per-kind count. Cons:
new broker code path, new verb, new ACK protocol — none small.

**B. New topic *name* pattern `tasks-<X>` over existing `agent-inbox`
kind.** Producer enqueues with `deliver_when=idle` and `protocol=task`.
Broker reuses existing dispatchAgentInbox, including the idle gate.
Cursor advances on UserPromptSubmit as today. Naturally serializes:
record N+1's idle gate doesn't open until the agent finishes acting
on record N. Pros: zero new broker code; just a name-pattern
auto-create entry. Cons: ACK is "started," not "done" — if the agent
mid-action sees the next task in inbox-immediate-style polling, it
could pull early. Mitigated by the idle gate, which won't dispatch
while agent is Working.

**C. work-queue with one consumer.** Use existing work-queue kind;
agent pulls one task, runs, pulls next. Pros: zero code. Cons: pure
agent-side polling, no broker push, no "task arrived" notification,
agent must remember the queue exists. Drift-prone.

**D. Per-agent task directory.** `$STATE/tasks/<agent>/{pending,
active,done}` with atomic renames. Pros: filesystem-native, no
broker changes. Cons: divorced from the bus event log and topic
registry — needs its own monitor surface. Reinvents what topics
already do well.

**E. coordination/ pattern.** Layer TodoWrite + hooks. Pros: no new
bus primitives. Cons: leaks claude-code-specific behaviour into a
bus concept; portability cost.

## Recommendation: **B** — `tasks-<X>` topic name over the existing
`agent-inbox` kind, with `bus task` as the user-facing verb sugar

Plumbing wise this is the smallest possible step that gives sulin the
conceptual separation. Broker change: extend the auto-create name
pattern (`topic_registry.cpp:getOrAutoCreate`) so `tasks-X` creates
an `agent-inbox` topic targeted at `X`, defaulting `deliver_when=idle`
and `protocol=task` on enqueue.

User-facing verbs (`src/sub/sub_produce.cpp` or sibling):

```
bus task enqueue NAME body...    # → enqueue to tasks-NAME, idle, task
bus task list NAME               # → peek tasks-NAME
bus task drop NAME ID            # → cancel a queued task (cursor skip)
```

Mailbox stays exactly as today. A producer who wants to chat sends
mail; a producer who wants to drop a work commitment sends a task.
The monitor's FOCUS column and a future per-kind mail count can show
"pending tasks: 3" distinctly from "pending mail: 1."

**When B isn't enough.** Two scenarios push us to A:

- *Tasks longer than one agent turn.* If a task spans many Stops with
  intermediate user interaction, UserPromptSubmit-as-ACK
  prematurely advances the cursor. Need an explicit `bus task done`
  protocol then.
- *Cancellable mid-flight tasks.* B can drop *queued* tasks but
  doesn't have language for "the active task should be abandoned and
  the next one dispatched." That's an A-level feature.

Both are deferrable. The everyday "serial N jobs" case sulin
described fits cleanly inside B.

## Out of scope for this proposal

- DAG / dependency-aware scheduling (sulin explicitly excluded this).
- Multi-consumer tasks (that's what work-queue is for).
- Cross-agent tasks (those compose from per-agent tasks via mail
  between agents).
