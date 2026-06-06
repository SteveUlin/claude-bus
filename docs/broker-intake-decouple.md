# Broker intake/processing decouple (Pillar D · W23 + W24)

**Status:** design — surfaced to auri for ack before implementation.
**Owner:** elodin. **Scope:** `src/rpc.{h,cpp}`, `src/broker.cpp`,
`src/delivery.{h,cpp}` (tick driver only). **Touches the RPC/tick/restart
spine** — so this is design-first per the "design before code on
restart/ack/cursor semantics" rule.

## 1. The problem

The broker is a **single-threaded pselect reactor**. One thread does two jobs
that fight each other:

- **Intake** — `accept()` a connection, `readLine` + parse one JSON-RPC
  request, run its handler, write the reply (`rpc::Server::serve`).
- **Processing** — every tick, `delivery::Loop::tick()` runs the whole
  delivery state machine: `scanEvents` (drain UPS acks), `scanRetries`,
  `maybeAutoClear`, `maybeScanTokens`, `maybeWakeIdleOffTty`, `maybeTrimLogs`,
  `dispatchAgentInbox`/`dispatchTuiCommands` per topic, `maybeEscalateStuck`.

These run on the **same thread**, so they share registry / topic logs /
cursors / in-flight tracker with **zero locking** — atomicity comes for free
because nothing is concurrent. That free atomicity is the property every other
pillar leans on (MPMC claim W12, cursor advances, graph projection W6/W10). It
is non-negotiable.

But the shared thread has two failure modes:

### 1a. The fan-out wedge (intake starved by processing)

The delivery tick forks `zellij` subprocesses — `paneId()` runs
`list-panes --json` (~1 s under a loaded zellij), `paneStateCached()` forks a
`dump-screen` (5 s-capped). Each is bounded individually, but the tick runs
them **per candidate agent**. Under a multi-agent fan-out the scan serializes
into tens of seconds. While the thread is inside the tick, it is **not back at
`accept()`** — the kernel recv-queue backs up, the listen backlog fills, and
clients can't even get a reply. The broker is alive and RPC-responsive by the
clock, yet effectively down. This is the live 2026-05-31 fan-out outage.

We have **band-aids** for this, all merged: the `kInnerBudget` 100 ms cap on the
RPC inner-drain loop (`rpc.cpp:236`), the `listPanesJsonCached` 3 s TTL
(`pane.cpp:462`), and the scoped doorbell pane-read that only forks for
boot-ambiguous agents (`delivery.cpp:1255`). Each narrows the window. **None
removes the structural coupling** — a slow-enough or numerous-enough set of
forks inside one tick still parks the only thread that can `accept()`.

### 1b. The RPC-driven-tick fragility (processing starved by quiet)

The flip side. With `pselect(timeout=250ms)`, the tick *should* fire every
250 ms on the idle path. In practice the idle timeout is EINTR-starved and
unreliable on a quiet fleet, so the tick effectively **advances on RPC
traffic** — viewer panes polling `state`/`inflight` at ~1 Hz are what actually
drive it. When nothing polls, time-based work stalls: retry escalation, PEL
idle-reclaim (W12), SLA timers (W21), the dead-man switch (W17), compaction
timing. D8 Part B already fixed *escalation* by arming a one-shot timerfd to
the next deadline (`rpc.cpp:149`) — this design **generalizes that one timerfd
to the whole tick** so every time-based action fires whether or not a client
is talking.

## 2. Goals / non-goals

**Goals**

1. Intake never blocks on a slow processing step. A burst of RPCs (or a quiet
   fleet) leaves intake free to `accept()` and reply — no recv-queue wedge.
2. Processing stays **single-consumer** — one thread owns all broker state, so
   today's lock-free atomicity holds *by construction*, not by discipline.
3. The processing tick is **self-driven** by a timerfd, independent of RPC
   traffic — fixing 1b for every timer, not just escalation.
4. Every blocking `zellij` op stays hard-capped, so a single pane call can
   never stall processing unboundedly.

**Non-goals**

- **Not** "make the broker multithreaded." Processing stays one consumer. This
  is a producer/consumer (reactor) split, not parallel processing.
- **No wire-format, cursor, ack, or epoch-fence change.** Restart semantics are
  byte-identical; only *where and when* the tick runs moves. (This is the
  reassurance the design-first rule asks for: §6 spells out that none of the
  durable semantics change.)
- **Not** async pane I/O. The tick still calls `zellij` synchronously (capped);
  we do not move pane forks to a worker pool. That's a possible future
  extension (§8), explicitly out of scope here to keep the change surgical.

## 3. Architecture: command-queue between two threads

```
        socket
          │  accept / readLine / parse                (INTAKE thread)
          ▼
   ┌──────────────┐   push {req, conn_fd}   ┌─────────────────────────┐
   │ intake loop  │ ─────────────────────▶  │  bounded command queue   │
   │ owns listen  │   signal eventfd        │  (mutex + eventfd)        │
   └──────────────┘                         └─────────────────────────┘
          ▲                                              │ drain
          │ wake to break accept on stop                 ▼
          │                                   ┌─────────────────────────┐
          └────────── gStopFlag ◀──────────── │  PROCESSING thread       │
                                              │  epoll{eventfd, timerfd} │
                                              │  • dequeue → run handler │
                                              │    → write reply, close  │
                                              │  • timerfd → tick()      │
                                              │  OWNS ALL BROKER STATE   │
                                              └─────────────────────────┘
```

**The key move:** the intake thread does **not** execute handlers. It parses a
request and hands the live `conn_fd` to processing via the queue. The
**processing thread** dequeues, runs the handler against its
exclusively-owned state, writes the response to that `conn_fd`, and closes it.

Why hand off the fd instead of running reads on intake? Because every handler
touches processing-owned state — `peek`/`body`/`state`/`inflight` read the
registry, topic logs, cursors, and the in-flight map; `enqueue`/`fetch`/`drain`
/`drop` mutate them. If intake ran *any* of them, it would race the tick. By
routing **all** ops through the queue, the processing thread remains the sole
reader and writer of broker state — **no locks on broker state at all.** The
only synchronized object in the whole broker is the command queue itself.

### What each thread may touch

| State | Intake | Processing |
|---|---|---|
| `listen_fd`, `accept()` | ✅ owns | — |
| command queue | push | drain (sole consumer) |
| a static "busy"/"shutting down" error string | ✅ (constants only) | — |
| registry, topic logs, cursors, in-flight, blocking_ops, scan offsets, token-scan, doorbell/strand/escalation state, epoch | — | ✅ owns exclusively |
| `conn_fd` write+close | only on backpressure-reject (§4) | ✅ normal path |

Intake touching only `listen_fd`, the queue, and string constants is what makes
the atomicity argument hold **by construction** (§6).

### Intake thread loop

```
loop while !stop:
  conn = accept(listen_fd)            // bounded: kernel backlog drains here
  req  = parse(readLine(conn))        // one-shot, local client; small read cap
  if !req: write errorResponse; close; continue
  if queue.full():                    // BACKPRESSURE (§4)
     write busyResponse(conn); close  // intake writes a *constant*, no state
     continue
  queue.push({req, conn})             // hand off fd; processing closes it
  // (does NOT close conn — processing owns it now)
```

`readLine` on a local one-shot client is effectively non-blocking, but to keep
the "intake never blocks" invariant airtight we give intake's `readLine` a
short read deadline; a client that connects and stalls mid-line is dropped
rather than parking the intake thread. (Same pattern as the existing
drain-side pipe deadline in `pane.cpp`.)

### Processing thread loop (epoll reactor)

```
epoll_add(eventfd)    // intake signals this on every push
epoll_add(timerfd)    // self-driven tick — replaces the rpc-loop timerfd
tick(); rearm()       // boot tick so first deadline arms (as today, rpc.cpp:180)
loop while !stop:
  epoll_wait({eventfd, timerfd})
  if eventfd readable:
     drain eventfd counter
     while cmd = queue.try_pop():
        resp = handlers[cmd.req.op](cmd.req)   // runs against owned state
        writeAll(cmd.conn, resp); close(cmd.conn)
     tick()                                     // dispatch promptly after intake
     rearm()
  if timerfd readable:
     drain timerfd
     tick()
     rearm()
```

Two things wake processing: **a new command** (so a freshly-enqueued mail
dispatches without waiting for a timer) and **a deadline** (so time-based work
fires on a quiet fleet). Both paths end in `tick()` + `rearm()`.

### `rearm()` — the self-driven cadence

Today `rearm` arms to `delivery::Loop::nextDeadlineMs()` only. Generalize:

```
arm timerfd to  min( now + kBaseCadenceMs , nextDeadlineMs() )
```

- `kBaseCadenceMs` keeps a periodic floor so the self-rate-limited scans
  (doorbell 5 s, token 5 s, trim, auto-clear) still get polled on a silent
  fleet. Keep **250 ms** initially to preserve current behavior; it can relax
  to 1 s later since deadlines are now explicit. The expensive work (zellij
  forks) only happens inside the gated branches, so a 250 ms empty tick is
  cheap.
- `nextDeadlineMs()` is unchanged from D8 Part B — the soonest pending turn /
  tool / retry deadline, escalate-once via the `*_alarmed_` sets. A past-due
  deadline clamps to "fire ~immediately," never 0 (same rule as `rpc.cpp:169`).

This is W24: the tick no longer needs anything to "poke an RPC." A
viewerless, attendee-less fleet still escalates, retries, reclaims, and runs
its dead-man switch on schedule.

## 4. Bounded queue + backpressure (the named trap)

The pillar's trap: an unbounded intake queue lets a fast producer outrun
processing and blow memory. Policy:

- **Bound depth** at `kQueueMax` (start 1024 commands). Bound by count, not
  bytes — requests are already line-capped by `readLine`'s `max_bytes`.
- **When full → reject, don't block.** Intake writes a constant
  `errorResponse("broker busy, retry")` and closes the connection. It does
  **not** block waiting for space, because blocking intake on a full queue
  reintroduces exactly the wedge we're removing: if processing stalls (even
  for a capped 5 s), a blocked intake stops draining the recv-queue.
  Rejecting sheds load and keeps `accept()` live. The CLI tools issue
  idempotent one-shot RPCs, so a "busy, retry" is safe to retry.
- **Why reject is safe to do from intake:** the rejection is a compile-time
  constant string — intake writes it without reading any processing-owned
  state, so the no-shared-state invariant holds.
- **Queue age (optional).** If we later add a max-age drop ("a command queued
  > T is stale, drop it"), it MUST use the monotonic clock (W1 / the
  suspend-fix clock), never wall-clock — a lid-close/resume jump must not
  mass-expire the queue. Not in the initial cut; depth-bound + reject is
  enough.

Backpressure is observable — but **NOT** via an `audit` topic append. Writing a
topic-log record is processing-owned state, which the intake thread must not
touch (the no-shared-state invariant just above — that's the whole reason reject
is safe from intake). So a reject **downgrades to a stderr log** (e.g.
`broker: backpressure — queue full, rejecting`) rather than an `audit` record. A
sustained busy condition surfaces in the broker's stderr / pane log, not the
audit topic. (rpc-1-0: an earlier draft of this section claimed an audit append
here, which would violate the invariant it sits under — corrected. If
audit-topic visibility is later wanted, intake must hand the reject to the
processing thread via the command queue to append; deferred — stderr is enough
for the initial cut.)

## 5. Shutdown & signals

Today: a signal handler sets `gStopFlag`; the single loop polls it; the `stop`
RPC handler also sets it. With two threads the handshake is:

- **Signal (SIGINT/SIGTERM):** handler sets `gStopFlag` (async-signal-safe
  atomic, unchanged). The intake thread — blocked in `pselect`/`accept` — sees
  EINTR, checks the flag, breaks its loop. It then signals the eventfd with a
  **stop sentinel** so the processing thread wakes from `epoll_wait`, observes
  `gStopFlag`, and exits its loop.
- **`stop` RPC:** now a queued command. Processing dequeues it, replies OK,
  sets `gStopFlag`, and self-exits the reactor; it also writes the eventfd is
  unnecessary (it's already awake). The intake thread notices `gStopFlag` on
  its next `accept` EINTR / loop check and breaks. (To avoid a hang if intake
  is parked in a clean `accept`, the processing side `shutdown(listen_fd)` or
  closes it to kick intake out of `accept` — chosen mechanism: processing sets
  the flag, then `::shutdown(listen_fd, SHUT_RDWR)` to force intake's `accept`
  to return.)
- **Drain on exit:** after the loops break, drain any remaining queued
  commands and reply "broker shutting down" (constant), so in-flight clients
  get a clean error instead of a reset connection. Then join the processing
  thread, then run the existing inode-guarded socket unlink (`~Server`) and
  `pid_path` unlink. Order: stop accepting → drain/reject queue → join → unlink.
  The inode-guard in `~Server` (`rpc.cpp:55`) is unchanged and still protects a
  successor broker's socket.

## 6. Why restart / ack / cursor semantics are unchanged

The reassurance the design-first rule demands — this is a **threading reshape
of the same state machine**, not a semantics change:

- **Single-consumer atomicity preserved.** Every mutation of registry, cursors,
  in-flight, and topic logs runs on the one processing thread, serialized
  through the queue. There is no second writer, so claim/cursor/ack atomicity is
  identical to today's single-threaded model. The queue serializes commands in
  arrival order per connection; cross-connection order is arrival order, same as
  today's accept order.
- **Epoch fence unchanged.** `current_epoch` is stamped by the `enqueue` handler
  and checked on dispatch exactly as now; both run on the processing thread.
- **In-flight load() / persistence unchanged.** `Loop::load()` still runs once
  at boot before the reactor starts; in-flight files and cursor files are
  written by the same code on the same thread.
- **Ack path unchanged.** `scanEvents` still tails `events.jsonl` from the
  processing thread; acks advance cursors exactly as today.
- **The only behavioral change is timing:** the tick now fires on a timerfd
  the broker owns, plus immediately after each command batch — so delivery is
  *more* prompt and *more* reliable on a quiet fleet, never less.

## 7. Rollout (phased, each verifiable in an isolated `CLAUDE_BUS_STATE`)

1. **Phase 0 — cap audit (DONE, no code change).** Audited every subprocess
   fork in `src/`. **Result: the processing path is already fully capped.**
   Both fork primitives — `runCapture` (used for `list-panes` + `dump-screen`)
   and `runSilent` (all three pane-write keystrokes via `sendToPane`) — route
   through `waitWithTimeoutOrKill` with `kDefaultSubprocessTimeout` (5 s,
   `pane.cpp:33`). The only uncapped fork is `agent_status.cpp::isFocused`'s
   `popen("zellij action list-clients")` — and it is **NOT in the processing
   path**: the broker gates presence via `hasPresenceFile` (a `stat`, no fork);
   `isFocused` is a viewer/CLI helper. So no fork the tick can reach is
   uncapped.
   **Decision: keep the 5 s cap, do NOT lower to 2 s.** Under the *current*
   single-threaded broker a transiently-slow (>2 s) `list-panes` under load
   would be SIGKILL'd → return -1 → read as "pane gone" → spurious
   mis-classification + skipped dispatch. That risks a real false-negative for
   a benefit Phase 1 makes moot: once decoupled, the cap bounds queued-RPC
   reply latency, not broker availability. 5 s stays the right headroom.
2. **Phase 1 — the split (IMPLEMENTED).** Added the bounded `CmdQueue` +
   eventfd in `rpc.cpp`; the intake thread now only accept/read/parse/enqueue
   (or reject-with-constant when full); a single processing thread drains the
   queue, runs every handler via `dispatch()`, writes each reply, and owns the
   timerfd tick. The old `kInnerBudget` inner-drain-budget loop is gone —
   intake no longer runs handlers, so there's nothing to budget. The base
   cadence (250 ms) ships in this phase too: processing pselects on
   `{eventfd, timerfd}` with a 250 ms floor, ticking after every command drain
   (prompt dispatch) and on every timer/idle wake (W24 quiet-fleet). So Phase 2
   folded into Phase 1 — `rearm()` already arms the processing reactor's
   timerfd to `nextDeadlineMs()`; the 250 ms floor stays as a safety net per
   auri's call.
3. **Phase 2 — cadence tuning (deferred, optional).** Whether to relax the
   250 ms floor toward 1 s now that deadlines are explicit. auri's call: keep
   250 ms (minimal change). No code pending.

No flag-gating of the threading change — it's structural. De-risked
sandbox-first per the workspace rule; never run against the live broker.

**Verification (all in isolated `CLAUDE_BUS_STATE`, rebuilt binary):**
- `tests/bus-itest.sh` — 45/45: full RPC round-trip (enqueue/peek/fetch/cursor/
  state/inflight/broadcast/pubsub/blackboard/body/TTL/blocking-op/boot-wipe)
  through the new queue. **Byte-identical behavior** — the §6 safety property.
- `tests/timerfd-itest.sh` — PASS: escalation fires on the timerfd alone with
  **zero RPC traffic**, now from the processing thread (W24).
- `tests/retention-itest.sh`, `tests/off-tty-itest.sh` — PASS: retention/cursor-
  rebase + off-TTY drain/strand/compact-skip semantics unchanged.
- `tests/decouple-itest.sh` (new) — PASS: with the processing thread parked in
  a capped fake-zellij fork and `CLAUDE_BUS_RPC_QUEUE_MAX=1`, a burst of RPCs is
  shed with "broker busy" while intake keeps accepting — **no connection
  refused/reset** (W23).
- `ctest` unit suite — PASS.

## 8. Out of scope / future

- **Async pane I/O.** If capped synchronous forks inside the tick still bound
  reply latency too loosely under extreme fan-out, move pane reads/writes to a
  worker-thread pool that posts results back to the processing queue as
  commands. Keeps processing single-consumer (results arrive as queued
  commands) while removing the last blocking op from the tick. Deferred — the
  cap + decouple should suffice first; measure before building.
- **signalfd in the epoll set.** Folds the SIGINT/SIGTERM handling into the
  processing reactor and removes the async-signal-safe + EINTR dance
  (improvement-roadmap §"signalfd in an epoll reactor"). Natural follow-on once
  intake/processing both run reactors.

## 9. Test plan

- **Self-driven tick (W24):** enqueue an `agent-inbox` record, issue zero
  further RPCs, assert dispatch within `kBaseCadenceMs`. Today's broker tests
  must "poke an RPC to drive a tick" (the rpc-driven-tick property); this test
  asserts that's no longer required.
- **Intake-not-starved (W23):** stub a slow (`sleep`) zellij op so a tick
  parks the processing thread; fire a burst of `ping`s; assert every `ping`
  replies under a tight bound while the tick is busy.
- **Bounded queue:** flood beyond `kQueueMax`; assert excess RPCs get the
  "busy, retry" error, no unbounded memory growth, and a **stderr backpressure
  log** per reject (NOT an audit record — see §4: intake can't append to a topic
  under the no-shared-state invariant).
- **Atomicity regression:** existing claim/cursor/ack tests must pass
  unchanged — they encode the invariant this design promises to preserve.
- **Shutdown handshake:** SIGTERM mid-tick → clean join, socket + pid unlinked,
  no orphaned processing thread; `stop` RPC → OK reply then exit.
