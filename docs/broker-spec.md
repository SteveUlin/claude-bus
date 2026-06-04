# The broker — async queued delivery (full spec)

Reference for the broker daemon. CLAUDE.md carries only the every-turn gist;
this file is the full specification, read on demand. It describes the broker as
the code actually behaves — earlier revisions of this file predated the off-TTY
default and the timerfd tick and described neither; this one is corrected.

`bus msg send` is the *raw* lever: it writes directly into a pane's TUI buffer
and submits immediately — useful for urgent unwedging but it contends with the
human keyboard, can interleave with other writers, and has **no ACK or retry**.
For all routine / queued / async delivery, go through the **broker daemon**.

## What the broker is

The broker (`bus broker run`) is the single source of truth for the bus. One
daemon, one processing thread. It owns:

- The topic registry (`$STATE/topics.json` — what topics exist, their kinds,
  per-kind config).
- Topic logs (`$STATE/topics/<name>.log` — append-only records, v4 wire format).
- Per-(topic, consumer) cursors (`$STATE/cursors/<topic>/<consumer>.cursor`).
- The in-flight tracker (`$STATE/in-flight/<msg_id>.json`), ack deadlines, and
  the audit / `inbox-human` escalation path.

Hooks ONLY emit state events to `events.jsonl` (via `settings/hooks/log-event.sh`)
and, for off-TTY agents, pull their own mail on a turn boundary (see Delivery).
Hooks never decide *when* to push — the broker does.

## The kernel (the whole design, stated up front)

Three primitives, and everything else is mechanism around them:

- An **append-only, byte-offset-addressed log** per topic. Records are immutable
  once written; offsets are stable forever.
- One **cursor per (topic, consumer) that advances ONLY on ACK** — never on
  dispatch. An un-acked record sits at the head and is simply re-evaluated next
  tick, so *retry is free*, not machinery.
- A **boot-epoch stamp** on every record. The broker bumps an epoch counter each
  boot and wipes volatile state; the stamp lets it fence off records written
  under a prior boot, so a surviving on-disk log can't re-deliver yesterday's
  mail after a restart.

Together these give at-least-once delivery, dedup, and restart-safety
simultaneously. One single-threaded processing reactor decides when a head
record may move.

**Where the epoch lives.** There is no dedicated epoch field yet. The boot-epoch
is stamped into the **low 8 bytes (little-endian u64) of each record's 16-byte
`correlation` field** — a field nominally documented as "RPC pairing." See
`stampEpoch()` / `recordEpoch()` in `src/delivery.h` and the wire layout in
`src/topic_log.h`. (The run-2 redesign proposes promoting this to a first-class
`epoch` field; until that lands, the overload is the reality and is documented
here so nobody mistakes a stamped record for an RPC-paired one.)

## Delivery model — off-TTY is the default

The fleet default is **off-TTY drain**, not the broker typing into panes:

- For an **off-TTY agent** (`isOffTty(agent)` in `src/tty_policy.h` — the
  compiled-in fleet default; opt-out is in that header, not `$STATE`), the
  broker marks the record ready and **does not write to the pane**. The agent's
  `UserPromptSubmit` hook calls `bus msg drain`, which pulls the agent's pending
  inbox records and injects them as Claude Code **`additionalContext`** on the
  next turn.
- For a **TTY agent** (opt-out of the default), the broker pushes into the pane
  via `sendToPaneSafe` — the safe write that preserves a human draft, defers on
  scrolled / locked / modal panes, and **flattens newlines to spaces** before
  writing (a raw `\n` reads as Enter in the prompt, so a multiline message would
  submit at its first newline and let a concurrent human typist's keys merge in;
  claude's prompt never holds a legit buffer `\n` — a soft newline is Shift+Enter
  at the key layer — so flattening is lossless for input). In practice **`comms`
  is the lone TTY agent** — the single human-facing opt-out; every other agent is
  off-TTY. The TTY arm exists precisely for `comms` and must be preserved (never
  collapse `comms` into the off-TTY arm).

Both paths converge on the same cursor + ack semantics below; they differ only
in the last-mile actuator. `bus msg send` remains the separate raw-TTY lever and
is outside this queued path entirely.

**This two-arm model IS the "Transport seam"** (broker-seam-redesign §6) — and
it stays a documented fork, not a component. The arm selector is `isOffTty`; the
TTY arm is `deliverInline` → `sendToPaneSafe`; the off-TTY arm is "don't push"
(the drain RPC pulls); the doorbell is the asleep-off-TTY branch. Those pieces
are already factored, so wrapping them in a `Transport` class would be ceremony
over a two-case fork — the seam's collapse-to-if tripwire, deliberately tripped.
The seam's real value (quarantining the off-TTY-vs-TTY model into one
authoritative place) is *this section*.

**The doorbell (waking an idle off-TTY agent).** An off-TTY agent only drains on
a turn boundary, so an *idle* one with queued mail would sit forever. The broker
rings a doorbell: `maybeWakeIdleOffTty` writes `[bus-wake]`, which fires the
agent's `UserPromptSubmit`, on which the drain hook delivers the mail as
`additionalContext`. The doorbell rings only an agent a pure predicate
`wakeReadyForMail(ax, pane)` (in `bus_agent_status`) judges ready:

| Agent state | Wakeable? |
|---|---|
| `Alive` + `Ready` | yes |
| `Compacting` | yes (post-compact strand fix) |
| (`Starting` \| `Stuck`) + pane mode `INSERT` | yes — **fresh idle at prompt** |
| (`Starting` \| `Stuck`) + pane not `INSERT` / not ok | no — preserves `BOOT_STUCK` |
| `Working` / `NeedsInput` / `New` / `Ended` / `Gone` | no |

The `INSERT`-rescue exists because from `events.jsonl` alone a fresh agent
waiting for its first prompt and a genuinely wedged boot are identical — both are
"`SessionStart`, no follow-up." Pane mode is the only disambiguator: an editable
prompt (`INSERT`) means ready; a startup modal/spinner means wedged. Without it,
a freshly-spawned peer's first brief strands until a manual `bus msg send` nudge
(harness-gap #4). `sendToPaneSafe` is still the final backstop — it refuses a
modal pane, so even a misread can't type into one.

## The tick — base cadence + deadline timerfd

There is **no single fixed 250 ms delivery loop**. The processing reactor wakes
on three sources, and runs the delivery pass on each wake:

1. **RPC arrival** — any client call (a send, a viewer's ~1 Hz poll) wakes the
   reactor immediately.
2. **Base cadence** — a `pselect` timeout, **250 ms by default**, is the *floor*:
   the longest the reactor sleeps with nothing else pending (`server.run(250ms,…)`
   in `src/broker.cpp`).
3. **Deadline timerfd** — a one-shot `timerfd` (CLOCK_MONOTONIC) armed after each
   tick to the **next ack/retry deadline**, so the reactor wakes exactly when a
   deadline is due rather than waiting out the cadence (`src/rpc.cpp`, the
   intake-decouple D8 reactor; see `docs/broker-intake-decouple.md`).

So 250 ms is a cadence floor, not the mechanism. Deadline-gated work (retry,
escalation) fires when its timerfd deadline arrives; some scans deliberately do
**not** run on every tick. Tests that depend on a tick must poke an RPC to drive
one — they cannot rely on an idle 250 ms beat alone.

## Topic kinds

| Kind | What it does |
|---|---|
| `agent-inbox` | Single recipient. Off-TTY: delivered via the drain hook as `additionalContext` (inline if body ≤ 1024 bytes; pointer + payload file if larger). TTY: pushed into the pane. Auto-created as `inbox-<name>`. |
| `tui-commands` | Single recipient. Broker dispatches via the dispatch state machine (`pane-state` READY check, normalize, retry). `/clear` and `/compact` mark the agent as having a blocking-op; subsequent delivery defers until the next `Stop` event. Auto-created as `commands-<name>`. |
| `work-queue` | Multi-consumer pull. Producers `bus msg enqueue`; consumers `bus msg fetch` (each fetch advances the cursor). |
| `pubsub` | Declared subscribers. On enqueue, broker cascades the record into each subscriber's `inbox-<name>` (canonical record stays on the pubsub topic for audit / replay). |
| `blackboard` | Last-value-wins. New writes fast-forward the cursor; readers `fetch` non-destructively. |
| `append-log` | Write-only audit. The broker uses `audit` itself for delivery-failure records. |

## Cursor semantics by kind

- `agent-inbox` / `tui-commands`: cursor advances on **ACK, not dispatch**. ACK
  comes from `events.jsonl`: `UserPromptSubmit` acks the oldest pending inbox
  record for that agent (off-TTY, the drain emits an explicit
  `{event:bus-ack,payload:{msg_id}}` per injected record); `Stop` acks a
  blocking-op slash. Until ACK, the record sits at the head and the in-flight
  gate prevents re-dispatch.
- `work-queue`: cursor advances on `bus msg fetch` (pull). Multiple consumers
  each get distinct records.
- `blackboard`: cursor stays at the latest record; `fetch` is non-destructive.
- `pubsub`: per-subscriber cursors on each `inbox-<sub>`; the original pubsub log
  keeps everything.

## Presence gate

The `[bus-attach]` sentinel controls presence: the broker defers ALL records for
an agent while `$STATE/presence/<agent>` is fresh. The cursor advances in FIFO
order with no per-record bypass.

All ack-driven cursor advances flow through **one** path — `Loop::onAck` →
`topic::advanceCursorMonotonic` — which writes the cursor **only forward**, so an
out-of-order or duplicate ack can never rewind it past already-delivered records.
The guard covers all three ack arms (blocking-op, bus-ack, positional
`UserPromptSubmit`); the blocking-op + positional arms were upgraded from
unconditional writes to monotonic in the `scanEvents`→`onAck` consolidation
(seam cut #2) — a **deliberate safety hardening**, observably identical on every
reachable trace (chronicler-verified, zero regressions).

## Reliability

Every dispatch creates an in-flight file with an ack deadline of
`$CLAUDE_BUS_ACK_TIMEOUT_MS` (default 60 s). What "retry" means on a missed
deadline differs by path, and the difference is load-bearing:

- **`agent-inbox` (TTY push):** a record is marked in-flight **only after**
  `sendToPaneSafe` actually wrote it, so `in-flight ⟹ already-delivered`. A
  missed deadline therefore **re-arms the deadline only — it does NOT re-send**.
  Re-pushing would append a byte-identical duplicate into the pane's input
  buffer (re-typing into a live pane is not idempotent); that was the
  dup-delivery bug. After `kMaxAttempts` (3) missed deadlines the broker
  **escalates, never re-delivers**: appends to the `audit` topic, mails
  `inbox-human`, advances the cursor past the record so the queue drains. A
  legitimately *deferred* delivery (pane scrolled/locked/gone) was never marked
  in-flight — it is re-attempted from the cursor on a later tick, a separate
  path from retry.
- **`agent-inbox` (off-TTY):** "re-delivery" is an idempotent re-`drain`, but
  off-TTY records set `next_retry_at = 0`, so the retry scan skips them entirely
  — the doorbell + drain handle redelivery, not the deadline timer.
- **`tui-commands` (slashes):** in-flight does **not** imply delivered here — a
  not-ready slash that never landed *should* legitimately retry. So this path
  still re-dispatches on a missed deadline, and `attempts` counts real
  re-deliveries. (Exactly-once for slashes is a tracked follow-up, not folded
  into the inbox fix.)

**Ack fragility (known follow-up).** Off-TTY acks by `msg_id` (the drain emits
an explicit `bus-ack` per injected record). The TTY path acks **positionally** —
any `UserPromptSubmit` acks the *oldest* in-flight record for the agent — so the
*cursor-advance* point is fragile (a UPS can burn against the wrong record).
This governs ack timing, not the dup root, which the per-kind rule above already
closes; tightening the TTY ack is a separate worthwhile fix.

## Retention & trimming

`$STATE` is durable (XDG root, survives reboot), so two log classes that once
vanished on a `/tmp` wipe now grow unbounded. The broker — the **only** writer to
topic logs, single-threaded on the same reactor as RPC — trims both in-tick, so
a rewrite never races a concurrent append.

- **`events.jsonl`** (advisory; the binary topic logs are canonical, so dropping
  old lines is safe): when it exceeds `CLAUDE_BUS_EVENTS_MAX_BYTES` (default
  16 MiB) the broker rewrites it to the most recent ~half (line-aligned, via
  tmp-file + atomic `rename`) — a **tail-preserving** trim, not roll-to-empty,
  because `readAgents` reads the whole file to derive per-agent state and a wipe
  would blank every agent's last-known state. After the rewrite the broker sets
  `events_offset_` to the new EOF so `scanEvents` resumes at the end and does
  **not** reprocess the tail (reprocessing an old `UserPromptSubmit` would
  positionally ack a newer in-flight record). Accepted caveat: a hook append
  racing the single `rename` writes to the unlinked inode and is lost — advisory
  data, self-heals on the next event.
- **Topic logs** (`$STATE/topics/<name>.log`, canonical): **head trim with cursor
  rebase**. `bus::retention::planTrim` (pure, unit-tested) picks a `cut_offset`
  from the larger of two limits — age (`retention_ms`, per-topic; `0` = none) and
  size (`CLAUDE_BUS_TOPIC_MAX_BYTES`, default 8 MiB; `0` = none) — by
  byte-slicing `header(64 B) + bytes[cut..EOF]` to a tmp file and `rename`-ing
  over the original (record bytes verbatim, timestamps intact — never re-append).
  Dropping `D` head bytes shifts every surviving absolute offset down by `D`, so
  the broker rebases each cursor and in-flight `cursor_after` for the topic via
  `new = max(header, old - D)`.

  **Delivery-guarantee clamp:** for `agent-inbox` / `tui-commands` the cut is
  clamped to `≤ min_cursor` — only records every consumer has passed are
  eligible, so **undelivered mail is never dropped** even if "expired" (an inbox
  with unread mail has `min_cursor = 0` → no trim; in-flight records sit at/after
  the cursor, so the clamp protects them). Advisory kinds (`audit` / `pubsub` /
  `work-queue` / `blackboard` / `append-log`) have **no clamp** — age/size expiry
  drops records regardless of consumption, which is what lets `audit.log` (no
  persistent consumer) actually shrink.

`retention_ms` is `0` for every auto-created topic today, so age-based expiry is
**dormant by default** — only the absolute `CLAUDE_BUS_TOPIC_MAX_BYTES` safety
cap is live. Set `retention_ms` on a topic to opt it into age expiry. (The
registry also declares a `max_record_bytes` field that **nothing enforces** —
dead config, flagged for removal by the run-2 redesign.)

## Lifetime & launch contract

The broker must be launched as a **direct child of zellij** — `layouts/fleet.kdl`'s
floating pane is the canonical path. `prctl(PR_SET_PDEATHSIG, SIGTERM)` ties the
broker's lifetime to that parent, so closing the pane / restarting zellij brings
it down cleanly.

Do **NOT** use `nohup`, `setsid`, or `disown` to background it. Any of those
defeat the parent-death signal and leave the broker reparented to init, where it
survives a zellij restart as an **orphan** — and the orphan still holds the
singleton flock, so it `DEFER`-blocks every new session's broker from starting,
silently, forever. (The singleton guard reaps such a corpse on startup; the
launch contract is what keeps you out of that state in the first place.) To
restart manually, prefer
`zellij action new-pane --floating -- /path/to/bus broker run`, or
`bus broker stop` + relaunch the layout.

## State layout

- `$STATE/topics.json` — topic registry.
- `$STATE/topics/<name>.log` — append-only topic logs (v4 wire format).
- `$STATE/cursors/<topic>/<consumer>.cursor` — per-consumer cursors.
- `$STATE/in-flight/<msg_id>.json` — in-flight tracker.
- `$STATE/presence/<agent>` — presence sentinel.

A per-boot wipe clears the volatile state (in-flight, presence) and bumps the
boot-epoch; the topic logs and cursors persist across boots, fenced by the
epoch stamp.
