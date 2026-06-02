# Dup-delivery fix — retry must not re-deliver an already-delivered record

Status: **accepted + implemented** (auri acked 2026-06-01; sulin pre-acked
on the output — regression green + land). The fix lands in this change.
Owner: elodin. Repro: bast (`tests/dup-delivery-itest.sh`) — flips to green
(exactly-once + escalation), verified 3/3.

## Symptom

A record was delivered **twice** to a TTY agent (prod trace
`1780377382788-auri-e9d7` → `inbox-comms`). `comms` is a hardcoded TTY
opt-out (`tty_policy.h:26`), so its mail goes through the TTY push path,
not the off-TTY drain path.

## Root cause — structural, not a timing race

The dup is not about ack timing, positional-vs-id acking, or intra-tick
ordering. It is structural:

1. **A record is marked in-flight ONLY after delivery succeeds.**
   `dispatchAgentInbox` (`delivery.cpp:643`):
   ```cpp
   if (!deliverInline(cfg_, agent, payload)) {
     // Write deferred (pane gone/scrolled/locked). Do NOT mark in-flight.
     return;
   }
   // ... mark in-flight, next_retry_at = now + ackTimeoutMs()
   ```
   `deliverInline` returns `true` only when `sendToPaneSafe` actually
   wrote the message to the pane. So **every in-flight agent-inbox record
   has already landed on the recipient's pane at least once.**

2. **`scanRetries` re-delivers in-flight records.** (`delivery.cpp:861`):
   ```cpp
   ok = deliverInline(cfg_, f.agent, payload);   // SECOND push → dup
   f.attempts += 1;
   f.next_retry_at = now + ackTimeoutMs();
   (void)ok;                                      // result ignored
   ```
   When no ack (UserPromptSubmit) arrives within `ackTimeoutMs`, the retry
   timer fires and **re-pushes the same bytes to the same pane** — a
   byte-identical duplicate. It repeats to `kMaxAttempts` (3), which is
   why bast's repro observes the record hitting the pane 3×.

**`in-flight ⟹ already-delivered`, so every retry re-push is a guaranteed
duplicate.** The ack only governs *when* the cursor advances; it cannot
prevent the dup, because the first retry that beats the ack has already
re-delivered.

### Why re-delivery is actively harmful on the TTY path

Re-typing into a live pane is not idempotent. The message may still be
sitting unsubmitted in the agent's input buffer; a re-push **appends a
second copy**, producing a doubled / garbled submission. (Contrast the
off-TTY path, where "re-delivery" is a re-`drain` that re-emits the same
`additionalContext` — genuinely idempotent. Off-TTY records set
`next_retry_at = 0` in `noteDrainDelivery`, so `scanRetries` skips them;
this bug is TTY-only.)

### The retry timer is conflating two different jobs

- **Deferred delivery** (pane scrolled/locked/gone): the record is *not*
  in-flight (line 643 returns before marking). `dispatchAgentInbox`
  re-attempts it from the cursor on a later tick. This is the legitimate
  "try again to deliver" case — and it is **not** handled by
  `scanRetries` at all.
- **Delivered-but-unacked** (in-flight): the record *did* land. The only
  thing missing is the agent's ack. Re-delivering does not help — the
  message is already there — it only dups.

`scanRetries` currently treats the second case as if it were the first.

## Proposed fix

**`scanRetries` must not re-deliver in-flight agent-inbox records.** The
retry timer becomes an **ack deadline**, not a re-delivery trigger:

- On each due tick, increment `attempts` (the deadline clock) but **do not
  call `deliverInline`**.
- At `attempts >= kMaxAttempts`, **escalate** exactly as today
  (`escalate(...)` → `inbox-ops`/audit, advance cursor, clear in-flight,
  `delivery.cpp:835-846`). The agent got the message once; if it never
  acked within `kMaxAttempts × ackTimeoutMs`, surface it to the human
  rather than spam the pane.
- An ack arriving any time before escalation advances the cursor and
  clears in-flight as today.

Net behavior: **a successfully-delivered agent-inbox record is pushed to
the pane exactly once; absence of an ack escalates, never re-delivers.**

This is surgical (it removes the re-`deliverInline` from `scanRetries`'s
agent-inbox branch and keeps the attempt/escalation bookkeeping) and loses
no legitimate retry, because deferred deliveries were never in-flight.

### Considered tradeoff

If a delivery *landed* (`sendToPaneSafe == true`) but the agent never saw
it (cleared the buffer, lost it), re-delivery would have been a second
chance. We judge this rare and dominated by the dup harm; the escalation
path (`inbox-ops`) is the correct safety net for "delivered, never acked."

## Secondary issue (note, not fixed here)

The TTY ack is **positional** (`scanEvents:458-490`): any `UserPromptSubmit`
acks the *oldest* in-flight record for the agent, and the event is consumed
one-shot. This makes the *cursor-advance* point fragile (a UPS can be
burned against the wrong record, or burned before its record exists). It
governs ack timing/correctness but is **not** the dup root — even a
perfectly-correlated ack cannot stop the first premature retry re-push.
The off-TTY path already replaced positional acking with ack-by-`msg_id`
(`scanEvents:434-454`, whose comment names this exact flaw). Tightening the
TTY ack (ts-correlation, or one-in-flight-per-TTY-agent) is a worthwhile
follow-up but is **out of scope** for the dup fix.

`scanRetries` also re-dispatches `tui-commands` (slashes). Re-running a
slash is a separate potential dup; not the reported bug, tracked separately.

## Test plan (bast)

`tests/dup-delivery-itest.sh` already reproduces the failing case (deliver
to a fake-zellij comms pane, no ack, count `sendToPaneSafe` write-chars per
`msg_id`). Lock two assertions:

1. **Exactly-once delivery:** push-count for the `msg_id` == 1, even with
   no ack and after several ticks past `CLAUDE_BUS_ACK_TIMEOUT_MS`.
2. **Escalation, not re-delivery:** after `kMaxAttempts × ackTimeout`, an
   escalation/audit record appears (`inbox-ops`) and the cursor advances —
   without a second pane write.

No "recognized ack" injection is needed for the core regression; the bug
fires in its absence.

## Resolved (auri rulings, 2026-06-01)

1. **Escalation-only on no-ack** — YES. Exactly-once is the goal, dups are
   the harm; delivered-but-unacked is ambiguous → surface to `inbox-ops`,
   do not re-send.
2. **Deadline window** — KEEP the duration (`kMaxAttempts × ackTimeoutMs`);
   don't change escalation latency as a side effect of a correctness fix.
   Representation left to the implementer: kept the `attempts` field as the
   deadline clock (smaller diff; a serialization rename would also mislead
   the tui-commands path, where `attempts` still counts real re-deliveries)
   with a clarifying comment, rather than a new field.
3. **tui-commands retry** — SEPARATE. This change is agent-inbox-scoped so
   bast's regression stays a meaningful green-gate. The slash fix is NOT a
   blind copy: tui-commands in-flight semantics differ (a not-ready slash
   that never landed *should* legitimately retry, so in-flight may not imply
   delivered there) — tracked as its own follow-up with its own repro.
