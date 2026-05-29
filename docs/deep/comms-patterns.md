# Communication Patterns & Resiliency — A Deep Reference for claude-bus

> Written 2026-05-28. The deep companion to `docs/modern-agent-techniques.md`,
> scoped to **one topic: how agents talk and how that conversation survives
> failure.** Every section leads with the **principle** — the mechanism, the
> tradeoff, the design pressure — then shows how the best implementations
> actually do it (with quoted source), then maps it to claude-bus. Skim the
> tables; read the synthesis at the end.
>
> The prior survey's thesis stands: claude-bus's moat is **durable,
> pane-backed, human-attachable agents**, and the flock'd TTY write is the
> load-bearing fragility. This document goes underneath that — into the
> *transport substrate, the messaging shapes, and the delivery guarantees* —
> and asks one question of our broker: **given that it is already a respectable
> local message bus, where do its typed topic kinds get the theory right, and
> where do they quietly violate it?**

---

## Part 0 — What our broker actually is (the honest model)

Before importing patterns, name the thing we have. Reading the source
(`src/broker.cpp`, `src/delivery.cpp`, `src/topic_log.cpp`,
`src/topic_registry.cpp`):

- **Transport substrate:** per-topic **append-only log files**
  (`$STATE/topics/<name>.log`), v4 binary wire format, single writer (the
  broker), readers seek by byte offset. Plus an **`AF_UNIX` socket** for the
  control plane (CLI ↔ broker JSON-RPC). Plus a **second append-only log**,
  `events.jsonl`, written by *hooks* (not the broker) carrying agent lifecycle
  facts.
- **Cursors:** one byte-offset file per `(topic, consumer)` at
  `$STATE/cursors/<topic>/<consumer>.cursor`, written atomically via tmp+rename
  (`writeCursor`, topic_log.cpp:334).
- **Message ID:** `{sent_ms:013}-{sender}-{rand:04x}` (topic_log.cpp:229) — a
  natural, sortable, near-unique idempotency key. **This is load-bearing and
  underused; remember it.**
- **Delivery model:** a **250 ms poll loop** (`Loop::tick`, delivery.cpp:769)
  that scans `events.jsonl` for ACKs, runs retry timers, then walks
  `agent-inbox` and `tui-commands` topics and *pushes* one record per topic per
  tick into the recipient's TUI via a flock'd `sendToPaneSafe`.
- **Guarantee:** **at-least-once with push + ACK-on-observation + 3-retry +
  dead-letter escalation.** The ACK is *inferred* from a downstream
  `UserPromptSubmit`/`Stop` event, not returned by the transport.

That last line is the crux of everything below. Our bus's **ACK channel is a
different log than its delivery channel**, joined heuristically by "the oldest
in-flight record for this agent." Every comms-resiliency idea worth stealing
either hardens that join or removes the need for it.

---

## Part 1 — Core principles & tradeoffs

### 1.1 The transport trilemma: socket vs log vs embedded-DB

**The principle.** A local message bus picks a substrate, and each substrate
fixes two of {low-latency, durable, queryable} at the cost of the third. There
is no free substrate; there is only the right substrate *per concern*.

| Substrate | Latency | Durable across restart | Queryable | Natural shape |
|---|---|---|---|---|
| `AF_UNIX` socket | sub-ms | **No** (state dies with process) | No | request/reply, liveness = connection |
| Append-only log file | ms (poll) / sub-ms (inotify) | **Yes** (crash-safe with `O_APPEND`) | scan-only | event stream, audit, replay |
| Embedded DB (SQLite-WAL) | sub-ms | **Yes** | **Yes** (indexed, CAS) | mutable coordination state |
| In-memory ring buffer | ns | **No** | filtered scan | hot pub/sub, bounded |

claude-bus already runs the first two — socket for control, log for topics —
and that split is *correct*. The interesting question is whether the third
(embedded DB) or fourth (ring buffer) earns a place. The comparable
implementations answer differently and the contrast is instructive:

- **Overstory** (jayminwest, archived 2026-05-28) is **all SQLite-WAL**: mail,
  merge queue, session state. One substrate, ACID, indexed, migratable.
- **Claude-fleet** (sethdford) runs a **two-tier hybrid**: durable SQLite
  blackboard *write-through* to an in-memory **Rust `RingBus`** for fast reads.
- **claude-bus** is **append-log + socket**, no DB, no ring.

The deep tradeoff: an append-only log is the *best* audit/replay substrate
(immutable, ordered, grep-able) and the *worst* query substrate (every
"how many unread for agent X" is an O(n) byte scan from a cursor — see our
`state` handler re-scanning the inbox log on every RPC, broker.cpp:498). SQLite
inverts that. **The principle is: use the log for the immutable fact stream,
use a DB for mutable coordination state you have to *query* or *atomically
mutate*.** We'll return to this in Part 4; for now, note that our cursors,
in-flight tracker, and blocking-op map are exactly "mutable coordination state
queried on a hot path," and they currently live as scattered files.

### 1.2 Push vs pull: who decides when a message is consumed?

**The principle.** Delivery is either **push** (producer/broker decides when the
consumer receives — server streams to subject) or **pull** (consumer decides
when — requests a batch on demand). The tradeoff is **liveness vs flow
control**: push minimizes latency but can overrun a busy consumer and races the
consumer's own input; pull gives the consumer perfect backpressure but adds
latency and tempts the anti-pattern of busy-polling.

NATS JetStream makes this an explicit consumer property and **recommends pull
for new projects** precisely because pull consumers self-regulate. Redis Streams
is pull-only (`XREADGROUP`). claude-bus is **push** for `agent-inbox` /
`tui-commands` (the broker types into the pane) and **pull** for `work-queue` /
`blackboard` (`bus msg fetch`).

The subtle thing claude-bus gets *exactly right* and most don't: **push into a
TUI is fundamentally racy** (it contends with the human keyboard and the
program's own line editor), so the broker pushes *one record per topic per tick*
and gates on agent readiness (`deliver_when=idle` → `computeState == Idle`,
delivery.cpp:507). That self-throttling is a pull-like discipline bolted onto a
push transport. The hybrid `/loop bus msg fetch inbox-<self>` fallback
(broker.cpp:609, the `fetch` handler that skips in-flight records) is the
project reaching toward "let the agent pull when push is unreliable" — and it's
the single most important design tension in our comms layer.

**Overstory resolves the same tension oppositely and it's worth studying.** It
runs *two* delivery paths from one SQLite mailbox:
1. **tmux mode** — a `UserPromptSubmit` hook runs `ov mail check --inject`
   before each prompt; the hook reads unread mail and injects it as
   `additionalContext`. **Pull, triggered by the agent's own turn boundary.**
2. **headless spawn-per-turn** — `ov serve` polls the mailbox; on unread mail it
   spawns a fresh `claude --resume <session>` and writes the batched turn to a
   real stdin pipe (`headless-mail-injector.ts`). **Push, but onto stdin, not a
   TTY** — race-free.

And critically, overstory **forbids agent-side Bash polling** with a runtime
detector (`mail-poll-detect.ts`): a `while`/`until` loop whose condition calls
`ov mail check` and whose body calls `sleep` is flagged as a "wait-poll" anti-
pattern. The lesson for our `/loop` fallback: **busy-poll-from-the-agent is a
smell; turn-boundary pull (the hook) is the principled version of the same
idea.**

### 1.3 Delivery guarantees: exactly-once is a lie, idempotency is the truth

**The principle.** Any system that retries on lost ACKs is **at-least-once** by
construction: if the ACK is dropped, you redeliver, and the consumer may see the
message twice. "Exactly-once delivery" does not exist at the transport layer;
the achievable, honest target is **at-least-once delivery + idempotent
processing**, where idempotency is enforced by a **stable per-message key the
consumer remembers**. NATS: `AckExplicit` + `AckWait` + `MaxDeliver`. Redis:
the message stays in the **Pending Entries List (PEL)** until `XACK`; an idle
entry is reclaimed by another consumer via `XCLAIM`/`XAUTOCLAIM`. Both are
at-least-once; neither promises exactly-once; both hand you the ID and tell you
to dedup.

claude-bus is **at-least-once and correct about it** — `scanRetries` re-dispatches
up to `kMaxAttempts=3`, then `escalate`s (delivery.cpp:731). But it has **no
consumer-side dedup**: nothing stops a retried `mail` from injecting the same
user turn twice if the first injection *succeeded* but its `UserPromptSubmit`
ACK was missed (e.g., the agent was mid-stream — see the "mid-stream silent
dropped turn" failure already in our memory). The ID exists; the dedup doesn't.
This is the highest-value gap in the comms layer and it's a few lines (Part 5,
#1).

### 1.4 Ordering and the FIFO-vs-priority tension

**The principle.** A queue is FIFO until someone wants a message to jump the
line; then you need **priority**, and priority *breaks* FIFO. The clean designs
make this an explicit per-message field and a documented sort, not an implicit
"urgent messages use a different path."

claude-bus is strict FIFO per topic (cursor advances in byte order; the
in-flight gate keeps the head record at the head until ACK). It has **no
priority field.** Both comparables do: overstory's `MailMessage.priority ∈
{low, normal, high, urgent}`; claude-fleet sorts reads by a `CASE priority`
ladder (`critical→high→normal→low`, blackboard.ts:165) then `created_at`. The
tradeoff claude-bus implicitly took: **FIFO + one-in-flight gives a clean ACK
join** (the oldest in-flight record is unambiguous). Adding priority would
complicate that join (the "oldest" record might not be the "next" record). This
is a *defensible* omission, not a bug — but it means an "urgent" interrupt today
has to use the bypass path (`bus msg send` direct-inject), which is exactly the
racy channel we're trying to retire. Worth a conscious decision (Part 5, #6).

### 1.5 Backpressure: bounded queues with an explicit full-policy

**The principle.** A producer faster than its consumer must do one of three
things: **block** the producer, **drop** messages, or **grow unboundedly** until
OOM. Only the first two are honest. The design choice is the *policy when full*,
and it must be explicit per channel: block / reject / drop-oldest / drop-newest.

- Redis Streams: `XADD ... MAXLEN ~ N` trims to ~N (drop-oldest, approximate).
- Claude-fleet RingBus: hard cap `MAX_MESSAGES_PER_TOPIC = 10_000`, and on
  overflow `channel.pop_front()` — **drop-oldest, exact** (ringbus/src/lib.rs).
- NATS: stream `MaxMsgs`/`MaxBytes` + a `Discard` policy (old/new).

claude-bus has `max_record_bytes` (per-*record* cap, default 4096) and
`kMaxRecordBytes` (1 MiB runaway guard) but **no per-topic queue-depth bound and
no full-policy.** For `agent-inbox` the natural backpressure is "one in-flight,
ACK-gated" (which we do, implicitly). But `work-queue` and the `audit` log can
grow without bound, and there's no documented policy. The fix is mostly
*naming* the policy each kind already implements, plus a depth cap on the pull
kinds (Part 5, #7).

---

## Part 2 — How the best implementations actually do it

### 2.1 Overstory — SQLite-WAL mailbox with a typed protocol

**The substrate.** One SQLite DB, WAL mode, tuned for multi-process access:

```ts
// src/mail/store.ts — createMailStore
db.exec("PRAGMA journal_mode = WAL");      // concurrent readers + 1 writer
db.exec("PRAGMA synchronous = NORMAL");    // safety/perf balance in WAL
db.exec("PRAGMA busy_timeout = 5000");     // retry 5s on lock contention
```

WAL is the whole reason SQLite works as a *bus*: multiple agent processes read
concurrently while the writer appends, and `busy_timeout` turns lock contention
into a bounded retry instead of an error. The schema is a single `messages`
table with a **runtime-derived CHECK constraint** on message type:

```ts
const TYPE_CHECK = `CHECK(type IN (${MAIL_MESSAGE_TYPES.map((t)=>`'${t}'`).join(",")}))`;
// columns: id PK, from_agent, to_agent, subject, body, type, priority,
//          thread_id, payload (JSON), read INTEGER, created_at
CREATE INDEX idx_inbox  ON messages(to_agent, read);   // unread-per-agent is O(log n)
CREATE INDEX idx_thread ON messages(thread_id);         // threading is indexed
```

Two things claude-bus's append-log *can't* cheaply do fall out for free here:
**`getUnread(agent)` is an indexed query**, not a byte scan from a cursor; and
**conversation threading** (`thread_id`, `getByThread`) is a first-class index.

**The typed protocol — the pattern most worth stealing.** Overstory's
`type` field is not a freeform string (our `protocol`); it's a closed union of
**semantic** and **protocol** types, each with a structured `payload`:

```ts
// src/types.ts
type MailProtocolType =
  | "worker_done" | "worker_died" | "merge_ready" | "merged"
  | "merge_failed" | "escalation" | "health_check"
  | "dispatch" | "assign" | "decision_gate";
interface MailPayloadMap {        // a payload interface per protocol type
  worker_done: WorkerDonePayload; worker_died: WorkerDiedPayload;
  decision_gate: DecisionGatePayload; /* ... */
}
```

So a mail isn't just text — it's a tagged union the *receiver can dispatch on*.
`worker_done` carries a result; `worker_died` lets a parent unblock instead of
waiting forever; `decision_gate` is a structured human-approval request. The DB
CHECK constraint enforces the closed set; the migration path (`migrateSchema`)
rebuilds the table when the set grows (SQLite can't `ALTER TABLE ADD
CONSTRAINT`). **This is the difference between "a bus that carries strings" and
"a bus that carries a protocol."** Our `protocol` field (`"text"`,
`"auto-clear"`, `"agent-end"`, `"delivery-failure"`) is reaching for this but
isn't a typed, payload-bearing union.

**The ACK model.** Overstory's headless injector marks mail read **only after**
the turn succeeds:

```ts
// headless-mail-injector.ts — mark-as-read happens AFTER runTurn returns
//   exitCode === 0 and no thrown error. On any failure, messages remain
//   unread and will be retried on the next tick.
```

This is at-least-once with **ACK = process-exit-success**, far stronger than our
ACK = "a UserPromptSubmit event appeared." And per-agent serialization is
enforced by a **turn lock** ("never spawn a second claude process for the same
agent"; "cross-process by the turn-lock inside `runTurn`") — the same
one-in-flight discipline our broker enforces via the in-flight gate, but
mechanized as a lock rather than inferred from a tracker.

**The work-queue.** `src/merge/queue.ts` is a textbook **SQLite FIFO with status
CAS**: `id INTEGER PRIMARY KEY AUTOINCREMENT` gives FIFO; `status ∈
{pending,merging,merged,conflict,failed}` is a state machine; `updateStatus`
transitions it. Claiming is an atomic conditional UPDATE (see claude-fleet below
for the explicit pattern).

### 2.2 Claude-fleet — two-tier blackboard (durable DB + hot ring buffer)

**The pattern: write-through caching for a message bus.** Every blackboard post
hits SQLite *and* an in-memory ring:

```ts
// src/storage/blackboard.ts — postMessage
stmt.run(id, swarmId, senderHandle, messageType, target, priority,
         JSON.stringify(payload), '[]'/*read_by*/, now);   // durable
const busTopic = `bb:${swarmId}:${messageType}`;
this.bus.publish(busTopic, senderHandle, priorityNum, JSON.stringify(payload)); // hot
```

The principle: **the durable substrate answers "what is true" (audit, replay,
crash-recovery); the hot substrate answers "what just changed" (low-latency
fan-out to subscribers).** Reads that need history hit SQLite; reads that need
*recency* hit the ring. This is the same split claude-bus has (log = truth,
in-flight tracker = recent) but claude-fleet makes the hot tier a real pub/sub
bus.

**Per-reader read tracking via a JSON set — multi-consumer done right.**
Instead of one cursor, claude-fleet stores `read_by` as a JSON array on each
message and filters unread per-reader in SQL:

```sql
-- unreadOnly for reader R:  message not yet read by R
NOT (read_by LIKE '%"' || ? || '"%')
-- markRead appends idempotently (the LIKE guard prevents double-insert):
UPDATE blackboard SET read_by = json_insert(read_by, '$[#]', ?)
  WHERE id = ? AND NOT (read_by LIKE '%"' || ? || '"%')
```

That `markRead` is **idempotent by construction** — re-marking is a no-op
because the WHERE clause excludes already-read messages. This is the
consumer-side dedup claude-bus lacks, expressed in SQL. (The `LIKE`-on-JSON is a
hack; a join table is cleaner; but the *idempotent-mark* idea is the takeaway.)

**The ring buffer — bounded queue with exact drop-oldest.** The Rust `RingBus`:

```rust
// crates/ringbus/src/lib.rs
const MAX_MESSAGES_PER_TOPIC: usize = 10_000;
let channel = self.channels.entry(topic).or_insert_with(VecDeque::new);
if channel.len() >= MAX_MESSAGES_PER_TOPIC { channel.pop_front(); } // O(1) evict oldest
channel.push_back(msg);
// plus: drain_old(max_age_ms) for time-based trimming
```

This is the **explicit backpressure policy** claude-bus is missing: a hard
depth cap, O(1) drop-oldest on overflow, plus age-based draining. `VecDeque`
makes both ends O(1).

**Atomic task claiming via status-CAS** (the multi-consumer-race fix). The
spawn-queue claims with conditional UPDATEs guarded on prior status, checking
`result.changes`:

```sql
UPDATE spawn_queue SET status='approved',  processed_at=? WHERE id=? AND status='pending';
UPDATE spawn_queue SET status='spawned', spawned_worker_id=?, processed_at=?
  WHERE id=? AND status IN ('pending','approved');   -- claim succeeds iff changes>0
-- dependency DAG: blocked_by_count decremented as deps complete
WHERE status='pending' AND blocked_by_count = 0       -- only runnable items
```

`changes > 0` ⇒ this process won the claim; `0` ⇒ someone else got there first.
The DB enforces single-assignment atomically across processes — **no two workers
pick up the same item.** This is the canonical fix for the multi-consumer
`work-queue` race, and it's the one place claude-bus is exposed (Part 4.3).

### 2.3 NATS JetStream — the reference vocabulary for push/pull + redelivery

NATS is the cleanest naming of the delivery-guarantee design space; adopt its
vocabulary even if not its code:

- **Ack policies:** `AckExplicit` (ack each — our model), `AckNone` (fire-and-
  forget — our `bus msg send` bypass), `AckAll` (ack N acks all prior — a
  *batch* optimization we could use for `broadcast`).
- **`AckWait` + `MaxDeliver` + `Backoff`:** redeliver if unacked within
  `AckWait`, give up after `MaxDeliver`. **Exactly our `ackTimeoutMs` +
  `kMaxAttempts`** — we independently reinvented JetStream's redelivery, which
  validates the design. The one piece we lack: **`Backoff`** (escalating retry
  delays). We use a flat `now + ackTimeoutMs` every retry (delivery.cpp:763); a
  backoff sequence would avoid hammering a wedged agent three times in 180s.
- **Durable vs ephemeral consumers:** durable consumers persist their cursor;
  ephemeral ones are GC'd after inactivity. Our cursors are durable files —
  good. But we have *no ephemeral-consumer cleanup*: a one-off `--consumer
  reviewer-run-3` leaves a cursor file forever.
- **Pull recommended for new consumers** because it self-regulates — the
  principle behind preferring our `/loop fetch` over broker push where the
  agent can self-pace.

### 2.4 Redis Streams — the PEL is the in-flight tracker, named

Redis Streams' consumer groups are **our in-flight tracker, productized**:

- **Pending Entries List (PEL):** every delivered-but-unacked message sits in
  the group's PEL. **This is exactly `$STATE/in-flight/<msg_id>.json`.**
- **`XACK`** removes from the PEL = our cursor-advance-on-ACK.
- **`XCLAIM` / `XAUTOCLAIM`:** another consumer takes ownership of an entry idle
  longer than `min-idle-time`. **This is the missing piece for fault-tolerant
  work-queues** — if a worker dies holding a claimed item, another reclaims it
  by idle time. claude-bus has no equivalent: a `work-queue` item `fetch`ed by a
  consumer that then crashes is simply *gone* (cursor advanced, never
  processed). XAUTOCLAIM-by-idle-time is the fix (Part 5, #8).
- **`XADD MAXLEN ~ N`:** approximate trimming = backpressure.
- **Dead-letter:** "after N retries, route to a special error queue" — our
  `audit` + `inbox-ops` escalation, exactly.

### 2.5 Kafka log compaction — the principled blackboard

**The principle.** Kafka offers two retention modes: delete (by time/size) and
**compaction** — "retain at least the last known value for each message key."
Old values for a key are garbage-collected; the latest survives. A **tombstone**
(a record with a null value) marks a key for deletion. This is the *changelog*
pattern: the log doubles as a queryable last-value-wins store while staying an
append-only log.

**This is precisely what our `blackboard` kind wants to be and isn't.** Today
blackboard "fast-forwards the `_default` cursor to the latest record"
(broker.cpp:411) — readers see only the newest, but **every superseded value
stays on disk forever**, and there's only *one* key per topic (the whole topic
is one cell). Log-compaction generalizes this: a blackboard topic with a **key
field** would let many cells coexist (`key=build-status`, `key=lock-holder`),
each last-value-wins, with a compaction pass reclaiming superseded records.
Tombstones delete a cell. That turns our single-cell blackboard into a proper
keyed KV-over-log (Part 5, #9).

### 2.6 Event sourcing / CQRS — the join we're missing

**The principle.** Event sourcing stores **facts** (append-only, immutable) and
**derives state** by folding a reducer over events past a cursor. CQRS splits
the *write model* (the event log) from *read models* (projections optimized for
queries). The discipline: state is a *pure function of the event stream*,
replayable and testable.

claude-bus is *accidentally* event-sourced — `events.jsonl` is a fact log — but
it **doesn't fold a reducer; it scans heuristically** (the prior doc's point,
and the root of "mid-stream dropped turn"). For *comms specifically*, the
event-sourcing lens exposes the deepest structural issue: **our ACK is a join
across two independent logs** (`topics/inbox-X.log` says "what was sent";
`events.jsonl` says "what the agent did"), correlated by the fragile heuristic
"oldest in-flight record for this agent acks on the next UserPromptSubmit"
(delivery.cpp:375-396). A real event-sourced design would have the *delivery
itself* emit an ACK event carrying the `msg_id`, closing the join exactly
instead of by-position-and-time (Part 5, #2).

---

## Part 3 — The design space, laid out

### 3.1 Messaging shapes vs claude-bus topic kinds

| Shape | Definition | Our kind | Verdict |
|---|---|---|---|
| **Request/Reply** | sender blocks for a correlated response | `correlation` field exists, **unused** (repurposed for epoch) | Missing. The wire format has the slot. |
| **Point-to-point inbox** | one named recipient, ordered | `agent-inbox` | Strong. The ACK join is the weak spot. |
| **Command channel** | imperative ops to one recipient, idle-gated | `tui-commands` | Strong; blocking-op state machine is genuinely good. |
| **Work-queue** | many consumers, each item once | `work-queue` | **Partially broken** — see 3.2. |
| **Pub/Sub** | declared subscribers, fan-out on publish | `pubsub` (cascade-on-enqueue) | Sound; fan-out-on-write is a valid choice. |
| **Blackboard** | shared cells, last-value-wins, no addressing | `blackboard` | Single-cell only; wants keying + compaction. |
| **Append-log / audit** | write-only, replayable | `append-log` | Correct by construction. |
| **Gossip / anti-entropy** | peers reconcile state epidemically | — | N/A for single-host; skip. |

### 3.2 The work-queue multi-consumer correctness hole

**The claim in CLAUDE.md:** "`work-queue` — Multi-consumer pull. Producers
`bus msg enqueue`; consumers `bus msg fetch` (each fetch advances the cursor).
Multiple consumers each get distinct records."

**What the code does** (broker.cpp:609 `fetch` handler): `fetch` reads the
cursor for `(topic, consumer)` where `consumer` defaults to `""` →
`"_default"`. It peeks one record from that cursor and advances it. So:

- If two consumers both call `bus msg fetch work-q` **with no `--consumer`**,
  they share the `_default` cursor. The broker serializes RPCs on one thread, so
  they won't corrupt the cursor — but they **race for records on a single
  cursor**: consumer A advances past record N, consumer B gets N+1. That's
  actually *distinct records* (good) — but it's **FIFO load-balancing on one
  cursor, not independent per-consumer streams.**
- If two consumers pass **distinct `--consumer` ids**, they get **independent
  cursors** → each sees *every* record (pub/sub-like fan-out, not work-sharing).

So "multiple consumers each get distinct records" is **only true if they share
the default cursor**, and "each fetch advances the cursor" is **only true
per-consumer-id**. The semantics flip on whether `--consumer` is passed — and
the doc describes one mode while the API exposes both, undocumented. There's no
**atomic claim**: the safety rests entirely on the broker's single-threaded RPC
serialization. The moment a consumer pulls *without* round-tripping the broker
(or if the broker ever multi-threads), two workers can grab the same item.
**Claude-fleet's `status CAS / changes>0` and Redis's `XCLAIM` both solve this;
we rely on an accident of the threading model.** (Part 5, #3.)

### 3.3 The two-log ACK join (the structural fragility)

A diagram of the actual data flow for one `bus msg mail alice "hi"`:

```
producer ──RPC enqueue──▶ topics/inbox-alice.log  (record, id=T-alice-R)
                                   │
broker tick: dispatchAgentInbox ───┤ peek head, gates pass
                                   ▼
                        sendToPaneSafe → flock'd TTY write into alice's pane
                                   │  (writes $STATE/in-flight/T-alice-R.json)
                                   ▼
        alice's claude reads the injected text as a user turn
                                   │
        UserPromptSubmit hook ─────┴──▶ events.jsonl  {event:"UserPromptSubmit", agent:"alice"}
                                   │
broker tick: scanEvents ───────────┤ "oldest in-flight for alice acks now"
                                   ▼
              writeCursor(inbox-alice, cursor_after); remove in-flight
```

The fragility: the ACK event **carries no `msg_id`**. The broker guesses *which*
in-flight record this UserPromptSubmit acks by picking the oldest-dispatched one
for that agent (delivery.cpp:377-387). This is correct *only if* exactly one
record is in flight per agent (which the one-in-flight gate mostly ensures) and
*only if* every dispatched injection produces exactly one UserPromptSubmit. Both
assumptions break in the known failure modes:

- **Mid-stream injection** (memory: "mid-stream silent dropped turn"): the
  injection lands while alice is mid-turn; the streamed text completes but the
  planned turn's tool calls drop, and the UserPromptSubmit may or may not fire →
  ACK misfires or never comes.
- **Human types at the same time**: a human-typed prompt produces a
  UserPromptSubmit that the broker will happily consume as the ACK for a pending
  mail, **acking a message the agent never actually read as mail.**

The clean fix is to make the ACK carry the id, which the hook *can* do (Part 5,
#2). This is the comms-layer version of the prior doc's "event-sourced reducer"
recommendation, applied to the delivery join specifically.

---

## Part 4 — Novel ideas worth considering

These go beyond "adopt X from repo Y" — they're combinations the comparables
hint at but none fully realize.

### 4.1 Self-acking delivery via the FIFO-drain hook (kills the two-log join)

Combine the prior doc's "control-FIFO drained by the agent via UserPromptSubmit"
with overstory's "inject as additionalContext." The agent's `UserPromptSubmit`
hook reads pending records from `$STATE/inbox/<agent>/` (or pulls via
`bus msg fetch`), injects them as context, **and emits an ACK event carrying the
exact `msg_id`s it consumed.** Now:
- delivery is race-free (no TTY write; context injection at a turn boundary),
- the ACK is *exact* (carries ids), not heuristic,
- and the consumer-side dedup is trivial (the hook records last-acked id per
  topic and refuses to re-inject — claude-fleet's idempotent `markRead`).

This collapses three of our problems (TTY race, heuristic ACK join, no dedup)
into one mechanism that uses *only documented hook behavior.* It's the comms-
specific synthesis of the prior doc's #1.

### 4.2 The in-flight tracker as a real PEL with idle-reclaim

Generalize `$STATE/in-flight/` from "agent-inbox push tracker" to a true Redis-
style **Pending Entries List** spanning all consumed-but-unacked records on
*every* kind, keyed by `(topic, consumer, msg_id)` with a `claimed_at`. Then a
single reaper (`XAUTOCLAIM`-style) handles redelivery uniformly: any entry idle
past its `AckWait` is re-dispatched (push kinds) or released for re-claim (pull
kinds). This unifies the three places we currently track liveness (in-flight
map, blocking-op map, retry timers) under one model and gives `work-queue`
crash-recovery for free.

### 4.3 Keyed, compacting blackboard (Kafka changelog over our append-log)

Add an optional `key` to blackboard records and a compaction pass: on enqueue,
fast-forward a *per-key* cursor; periodically rewrite the log keeping only the
latest record per key (+ tombstones to delete). This turns one topic from a
single cell into a queryable KV store *while staying an append-only log* — no
SQLite needed, and it bounds blackboard growth (today superseded values
accumulate forever). Readers `fetch --key build-status` get last-value-wins per
cell.

### 4.4 Typed protocol envelopes with receiver-side dispatch

Promote our freeform `protocol` string to overstory's tagged-union model: a
small closed set of protocol types (`worker-done`, `escalation`, `decision-
gate`, `health-check`, `dispatch`) each with a documented JSON payload schema,
and a receiver convention (a skill or hook) that dispatches on type. The
difference is whether a peer message is "text the human/agent reads" or "a
structured event the agent's tooling acts on." Our `dispatch` skill already
implies a protocol; naming it makes fan-out/gather/judge-panel patterns
mechanical instead of prose.

### 4.5 Request/reply over the unused `correlation` field

The wire format reserves 16 bytes for `correlation` (topic_log.h:69), currently
hijacked for the broker epoch (8 of those bytes). A real correlation id would
let `bus msg ask alice "..."` enqueue with a fresh correlation id and block (or
poll) for a reply record on the sender's inbox carrying the *same* correlation
id — closing the "no reply channel" gap CLAUDE.md documents as a known
limitation. The slot exists; only the epoch squats on it. (Decouple epoch into
its own field first.)

---

## Part 5 — How this maps to claude-bus

Principle-first, each with rough effort/payoff. The unifying theme: **our bus
got the *topology* right (typed kinds, durable log, cursors, retry+DLQ) and the
industry agrees — what it's missing is exactness in the ACK join, dedup,
multi-consumer atomicity, and explicit backpressure.** Spend effort there, not
on a substrate rewrite.

### Flaws spotted in current code (with refs)

1. **No consumer-side idempotency dedup.** A retried delivery whose first
   attempt *succeeded* but missed its ACK re-injects the same turn
   (`scanRetries`, delivery.cpp:744-766; `kMaxAttempts` re-sends with no
   dedup). The `msg_id` exists (topic_log.cpp:229) and is never used as a dedup
   key by any consumer.
2. **ACK join is heuristic, not exact.** `scanEvents` acks "the oldest
   in-flight record for this agent" on *any* `UserPromptSubmit`
   (delivery.cpp:375-396). A human-typed prompt or a misfired mid-stream
   injection acks the wrong record. The event carries no `msg_id`.
3. **work-queue multi-consumer semantics are undocumented and CAS-free.** The
   `fetch` handler (broker.cpp:609) flips between load-balancing (shared
   `_default` cursor) and fan-out (per-`--consumer` cursors) based on whether
   `--consumer` is passed; CLAUDE.md documents only the former. No atomic claim
   — single-assignment relies on the broker's single-threaded RPC loop, not on
   the data model.
4. **No idle-reclaim for pull kinds.** A `work-queue` item `fetch`ed by a
   consumer that crashes before finishing is lost — the cursor advanced, no PEL
   entry, no XCLAIM equivalent. Only *push* kinds get retry.
5. **Flat retry, no backoff.** `scanRetries` sets `next_retry_at = now +
   ackTimeoutMs` every attempt (delivery.cpp:763); a wedged agent gets hit 3×
   at a fixed interval. NATS-style `Backoff` would space them.
6. **No priority field.** Strict FIFO means an urgent interrupt must use the
   racy `bus msg send` bypass; both comparables have priority.
7. **No per-topic depth bound or full-policy.** `max_record_bytes` caps a
   record, not a queue. `work-queue`/`audit` grow unbounded; no drop-oldest
   like RingBus's `pop_front`.
8. **DLQ is not re-drivable.** `escalate` (delivery.cpp:657) appends to `audit`
   + `inbox-ops` and advances the cursor — a dead end, not a loop. (Prior doc
   noted this too.)
9. **Blackboard leaks superseded values + is single-cell.** broker.cpp:411
   fast-forwards the cursor but never reclaims old records; one topic = one
   cell.
10. **`body` RPC and `drop` scan *every topic's* full log** (broker.cpp:574,
    682 call `log.dump()` per topic in a loop) — O(total bus bytes) per call.
    An append-log substrate makes "find msg by id" inherently expensive; an
    index (or the SQLite read-model) fixes it.

### Recommendations, ranked by payoff ÷ effort

| # | Move | Principle | Effort | Payoff |
|---|---|---|---|---|
| **1** | **Consumer-side dedup keyed on `msg_id`** (record last-acked id per topic in the drain hook; refuse re-inject) | At-least-once is only safe with idempotent processing; we have the key, not the check | **Low** | **Very High** |
| **2** | **Make the ACK carry the `msg_id`** — drain hook / injection emits `{event:"bus-ack", msg_id}`; `scanEvents` matches by id, not by oldest-in-flight | Close the two-log join exactly; kills mid-stream + human-types ACK misfires | Med | **Very High** |
| **3** | **Atomic claim for work-queue** — a `claim`/`status` field with conditional-advance (`changes>0` semantics), document the load-balance vs fan-out modes explicitly | The DB/CAS enforces single-assignment; don't rely on the threading model | Med | **High** |
| **4** | **Generalize in-flight → a PEL with idle-reclaim** for all kinds (XAUTOCLAIM by `claimed_at` + `AckWait`) | Unify retry/liveness; give pull kinds crash-recovery | Med | High |
| **5** | **NATS-style backoff** in `scanRetries` (e.g., 1×, 2×, 4× `ackTimeoutMs`) | Don't hammer a wedged consumer; space redelivery | **Low** | Med |
| **6** | **Priority field on the wire** + documented sort; reserve for genuine interrupts so they stop using the racy bypass | FIFO+priority is the honest model; retire the bypass path | Med | Med |
| **7** | **Per-topic depth cap + named full-policy per kind** (inbox=one-in-flight; work-queue=reject-on-full; blackboard=drop-all-but-latest; audit=age-trim like `drain_old`) | Bounded queues with explicit policy are the only honest design | Low | Med |
| **8** | **`bus msg redrive <msg_id>`** to re-enqueue a dead-lettered record | Turn the DLQ from a dead end into a loop | **Low** | Med |
| **9** | **Keyed, compacting blackboard** (Kafka changelog over the append-log) | Last-value-wins *per key* without a DB; bounds blackboard growth | Med | Med |
| **10** | **SQLite read-model for coordination state** (cursors, in-flight, msg-by-id index) — keep the append-log as the write model (CQRS) | Log = immutable truth; DB = queryable mutable state. Fixes the O(n) scans (#10 above). Only if the scans actually hurt. | High | Med |
| **11** | **Typed protocol envelopes** (overstory's tagged union) for protocol messages | A bus that carries a protocol, not strings; makes dispatch/gather mechanical | Med | Med |
| **12** | **Request/reply via the `correlation` field** (decouple epoch first) | Close the documented "no reply channel" gap; the slot exists | Med | Low–Med |

### What to deliberately NOT do

- **Don't replace the append-log topic substrate with SQLite.** Overstory is
  all-SQLite and pays for it with schema migrations (`migrateSchema`,
  `migrateBeadIdToTaskId`) and lost grep-ability. Our log is crash-safe,
  replayable, auditable — exactly what a fact stream wants. Add SQLite as a
  *read-model* (CQRS) for queries, not as the system of record.
- **Don't add an in-memory ring tier yet.** Claude-fleet's two-tier shines at
  swarm scale (many subscribers, high message rate). On a single host with a
  handful of panes, the 250 ms poll (or the inotify loop the prior doc
  recommends) is fast enough; a ring is complexity you won't recover.
- **Don't chase exactly-once.** It's a myth at the transport layer. Ship
  at-least-once + dedup (#1) and stop.
- **Don't add priority before fixing the ACK join (#2).** Priority complicates
  "which record acks" precisely because it breaks the oldest-in-flight
  heuristic. Exact-id ACK is the prerequisite.

---

## Sources

**Read at source (gh API, full files quoted above)**
- [jayminwest/overstory — `src/mail/store.ts`](https://github.com/jayminwest/overstory/blob/main/src/mail/store.ts) — SQLite-WAL mailbox, runtime CHECK constraint, schema migration, WAL pragmas
- [overstory — `src/types.ts`](https://github.com/jayminwest/overstory/blob/main/src/types.ts) — typed mail protocol union + per-type payload interfaces
- [overstory — `src/agents/headless-mail-injector.ts`](https://github.com/jayminwest/overstory/blob/main/src/agents/headless-mail-injector.ts) — mark-read-after-success ACK, per-agent turn lock, stdin injection, metadata escaping
- [overstory — `src/agents/mail-poll-detect.ts`](https://github.com/jayminwest/overstory/blob/main/src/agents/mail-poll-detect.ts) — runtime detector forbidding agent-side Bash mail-polling
- [overstory — `src/merge/queue.ts`](https://github.com/jayminwest/overstory/blob/main/src/merge/queue.ts) — SQLite FIFO work-queue with status state machine
- [sethdford/claude-fleet — `src/storage/blackboard.ts`](https://github.com/sethdford/claude-fleet/blob/main/src/storage/blackboard.ts) — durable+ring two-tier, `read_by` per-reader tracking, idempotent `markRead`, priority sort
- [claude-fleet — `src/workers/message-bus.ts`](https://github.com/sethdford/claude-fleet/blob/main/src/workers/message-bus.ts) — topic ring-buffer pub/sub, JS fallback, 10k cap
- [claude-fleet — `crates/ringbus/src/lib.rs`](https://github.com/sethdford/claude-fleet/blob/main/crates/ringbus/src/lib.rs) — `VecDeque` O(1) drop-oldest eviction, `drain_old`
- [claude-fleet — `src/storage/spawn-queue.ts`](https://github.com/sethdford/claude-fleet/blob/main/src/storage/spawn-queue.ts) — atomic status-CAS claiming (`changes>0`), dependency DAG via `blocked_by_count`

**Messaging-system design docs**
- [NATS JetStream Consumers](https://docs.nats.io/nats-concepts/jetstream/consumers) — AckExplicit/None/All, AckWait, MaxDeliver, Backoff, durable vs ephemeral, pull-recommended
- [Redis Streams](https://redis.io/docs/latest/develop/data-types/streams/) + [XCLAIM](https://redis.io/docs/latest/commands/xclaim/) / [XAUTOCLAIM](https://redis.io/docs/latest/commands/xautoclaim/) / [XACK](https://redis.io/docs/latest/commands/xack/) — PEL, consumer groups, idle-time reclaim, MAXLEN trimming, dead-letter-after-N
- [Single-shot reliable consumers with XREADGROUP CLAIM (Redis 8.4)](https://redis.io/blog/single-shot-reliable-consumers-with-xreadgroup-claim-in-redis-84/)
- Apache Kafka log compaction — retain-last-value-per-key, tombstone deletes, changelog pattern (Kafka docs, `#compaction`)

**Concepts**
- Event sourcing / CQRS — facts as the write model, projections as read models, reducer-folded state
- [mattbishop/sql-event-store](https://github.com/mattbishop/sql-event-store) — dedup + ordering in a SQL event store (idempotency keys)

**claude-bus source read for this doc**
- `src/broker.cpp` (enqueue/pubsub-cascade/blackboard cursor/fetch/drop/body/state handlers)
- `src/delivery.cpp` (dispatch loop, scanEvents ACK join, scanRetries, escalate, gates)
- `src/topic_log.cpp` / `.h` (v4 wire format, msg-id, cursors)
- `src/topic_registry.h` (topic kinds + per-kind config)
- `settings/hooks/log-event.sh` (the events.jsonl writer)
