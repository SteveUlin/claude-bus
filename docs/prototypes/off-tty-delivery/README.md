# Off-TTY delivery — prototype

Author: elodin · 2026-05-28 (overnight) · For: sulin to review
Status: **prototype, not wired into anything live.** Roadmap 2.1 /
`docs/deep/transport.md` §5.1, §6.2.

## What this is

A runnable demonstration of the highest-leverage structural move on the
board: **deliver agent-inbox mail off the TTY.** Today the broker types
mail into the pane (`pane.cpp` screen-scrape + flock'd TTY write), which
races the program's reads, the human's keyboard, and the line-editor
mode — the root of every delivery bug in memory (mid-stream dropped
turn, broker wedge, `detectMode` scar tissue).

The off-TTY shape inverts it: the broker writes a file and **never
touches the TTY**; the agent drains its own inbox at a clean turn
boundary via a `UserPromptSubmit` / `SessionStart` hook and emits the
records as `additionalContext`. The pane stays the human's.

This is the shape Anthropic's Agent Teams and HCOM both use, and the
single change three of the deep-dive docs converge on.

## Run it

```sh
docs/prototypes/off-tty-delivery/demo.sh
```

Fully isolated — a throwaway `$STATE` under `/tmp`, no broker, no fleet
state, no `settings.json`. It walks four behaviors:

1. **Delivery** — broker writes two records; a `UserPromptSubmit` drain
   emits both as one `additionalContext` block.
2. **Idempotency** — a second drain emits nothing (cursor + per-msg_id
   dedup). At-least-once transports redeliver; this refuses the dup.
3. **Presence gate** — with the `[bus-attach]` sentinel fresh, the drain
   **defers** (emits nothing) — the human has the keyboard.
4. **Deferred-then-delivered** — on detach, the held record delivers.

## Files

| File | Role |
|---|---|
| `inbox-drain.sh` | The hook. Drains past a cursor, honors the presence gate, dedups by msg_id, emits `additionalContext`. The heart of the prototype. |
| `sim-broker-write.sh` | Stands in for the broker's delivery side (appends one NDJSON record). The real broker would `TopicLog::append` to `inbox-<agent>`. |
| `demo.sh` | Isolated end-to-end driver. |

## Mapping to the real broker

The prototype reads a simple NDJSON file so it's shell-only and
self-contained. The real integration changes **only the record source**;
the drain logic (cursor, presence gate, idempotency, framing) is
identical:

- **Record source.** Instead of `inbox-proto/<agent>.ndjson`, the hook
  reads the existing `inbox-<agent>` topic log (v4 binary wire format).
  A bash hook can't parse that, so add a thin read verb — e.g.
  `bus msg drain <self>` — that reads from a dedicated `hookdrain`
  consumer cursor, prints pending records as NDJSON, and advances that
  cursor. ~30 lines of C++ reusing `topic::parseFrom` (now exposed +
  tested by Piece 1) and `topic::{readCursor,writeCursor}`. Read-only
  with respect to delivery — it does not change the broker.
- **The ACK is free.** The `UserPromptSubmit` event the hook fires on is
  the *same* signal `delivery.cpp::scanEvents` already consumes to
  advance the broker's cursor (lines ~350-382). No new ACK plumbing —
  though making the ACK carry the msg_id (roadmap 2.3) would close the
  two-log positional-join hole and pairs naturally with this drain.
- **Bootstrap.** `SessionStart` runs the same script (pass
  `SessionStart` as `$1`) to flush any backlog accumulated while the
  agent was down — `additionalContext` there lands "before the first
  prompt." `UserPromptSubmit` covers steady state. A genuinely
  idle-but-alive agent (no turns, nothing queued at boot) still needs a
  wake; see open questions.
- **Presence stays load-bearing.** `dispatchAgentInbox` /
  `dispatchTuiCommands` early-return on `hasPresenceFile` today; that
  gate moves *with* delivery to the hook (§6b.4). The prototype mirrors
  the 1h freshness expiry.

## What this deliberately does NOT do

- **Not wired into `settings.json`.** The live `UserPromptSubmit` /
  `SessionStart` hooks are unchanged. Nothing about the running fleet's
  behavior changes.
- **Does not touch the broker or the live delivery path.** No C++ built,
  no `bus` verb added yet — that's the first real-integration step, left
  for sulin to green-light.
- **Broker-side cursor/ACK/epoch/retry untouched.** The prototype only
  demonstrates the agent-side drain; it does not replace
  `deliverInline` → `sendToPaneSafe`.

## Open questions for sulin

1. **The idle-but-alive wake.** A pull channel only delivers when the
   agent takes a turn. Boot backlog → `SessionStart` drain (free). Steady
   state → `UserPromptSubmit` drain (free). But an agent sitting idle
   with no turns needs a nudge. Options: a single signal-only TTY
   keystroke (a "doorbell," not the payload — shrinks the racy surface
   to one byte), or a `Monitor` on `tail -F inbox` that crosses
   tool-call boundaries (Agent-Teams-like, but the agent must invoke it
   at SessionStart). My lean: doorbell for v1, it reuses the one TTY
   write we can't avoid and keeps the payload off-TTY.
2. **Keep push as fallback, or go pull-primary?** `delivery-alternatives.md`
   recommended push-primary + `/loop` fallback. transport §5.3 argues to
   invert it: pull-primary, TTY only for the doorbell. This prototype
   assumes pull-primary. Worth a decision before integration.
3. **`additionalContext` schema.** I used
   `{hookSpecificOutput:{hookEventName, additionalContext}}` per the
   transport doc. Confirm against the live hooks reference before wiring
   (couldn't fetch docs in the autonomous run).
4. **Multi-record framing.** The prototype concatenates all pending
   records into one `additionalContext` block per turn. Is one combined
   injected turn right, or one turn per record? One block is cheaper and
   matches "drain on this turn"; per-record would need turn-boundary
   coordination the hook can't do.

## Next integration step (when approved)

Add `bus msg drain <self>` (read-only verb over `inbox-<self>` with a
`hookdrain` cursor), point `inbox-drain.sh` at it instead of the NDJSON
file, prototype it on **one** agent (elodin) by adding the hook to a
single workspace's settings before fleet-wide. Pair with the msg_id ACK
(2.3) and keep the presence gate.
