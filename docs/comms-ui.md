# comms-UI: structured "who's working on what" surface for sulin

`bus log` is a time-ordered scroll of fleet events — useful for
debugging, wrong for the cockpit. sulin's words: *"the #1 thing I need
is who is working on what, but a more structured view."* What's
structured here is the **agent**, not the timeline. We need a present-
state per-agent view that pins what needs human action and dims
ambient activity.

## What's missing today

- `bus log` reads agent-by-time. Useful when debugging dispatch but
  noisy for the human.
- `bus monitor` is one-line-per-agent. TITLE was the right idea but a
  single column can't hold context.
- Neither surface pins "agent X needs you to answer Y" — that signal
  has to be inferred from `NEEDS_INPUT` state in the dashboard.

The gap: a **per-agent stat-block** view that reads like a hand of
cards, with `NEEDS YOU` at the top and ambient agents dim at the bottom.

## Proposed surface — `bus deck`

One stat-block per agent. Two zones: ⚠ NEEDS YOU (pinned, prominent),
then ✓ FLEET (sorted: WORKING → IDLE → STARTING; dim the further
toward IDLE). Sample frame:

```
⚠ NEEDS YOU ────────────────────────────────────────────
  elodin    broker auto-clear gate
    asks   "should the auto-clear gate on epoch shift?"
    in     broker.cpp · asked 2m ago

✓ FLEET ───────────────────────────────────────────────
  kvothe   design comms UI                       ◇ working
    last   "shipped bus log a41bc756" (30m)
    in     claude-bus · commit fe2d749d
  auri     hub coordination                      ◇ working
    last   "dispatched #44 to kvothe" (60s)
    in     claude-bus
  bast     fleet.kdl layout iteration            💤 idle
    last   "comms-bar working on layout" (5m)
    in     claude-bus
```

Fits ~12 agents in a 25-row pane. Re-renders on 1 Hz tick. The
`NEEDS YOU` zone disappears entirely when no agent is in
`NEEDS_INPUT` — no empty header.

## Signal mapping

Per agent, four fields:

| Field   | Source                                                     |
| ------- | ---------------------------------------------------------- |
| title   | `$STATE/title/<agent>` → focus file → "—"                  |
| last    | most recent `Stop` `last_assistant_message` first-line in tail |
| asks    | most recent `Notification(permission_prompt)` payload      |
| in      | `payload.cwd` basename + most recent file edit (optional)  |

All four come from sources `bus monitor` already reads (events.jsonl
tail + focus/title files). No broker change. No new state.

## Sorting + filtering

- Top zone: agents with state `NEEDS_INPUT` (sorted by oldest pending).
- Bottom zone: WORKING (recent activity first) → IDLE → STARTING.
- Hide `GONE` / `ENDED` unconditionally; this is a cockpit, not a
  historian. (Use `bus state --all` to inspect tombstones.)
- `bus log` stays — it's the debugger view. We don't delete it.

## Where it lives

Replace the `bus-log` pane in `layouts/fleet.kdl` (the right column
above `jj-log`) with `bus deck`. `bus log` stays available as a verb
but isn't pinned to the cockpit. monitor keeps its row — it answers
"is the broker alive, what's the rough fleet shape" cheaply; `bus
deck` answers "what should I do next."

## Out of scope (v1)

- Interactive: no key bindings, no drill-down. Read-only render.
- Threading: each agent's "last" is one message, not a thread. If a
  thread view becomes load-bearing later, that's a separate verb.
- Cross-agent context (e.g., "auri dispatched to kvothe; kvothe
  hasn't replied"). Hub-coordination concerns belong to auri's
  surface, not the cockpit deck.

## Open for sulin / comms

- `bus deck` or another name? (alternatives: `bus brief`, `bus desk`)
- Should the `in` line include the most recent file edited, or just
  the project basename?
- Refresh cadence: 1 Hz like monitor, or event-driven via inotify on
  events.jsonl + focus/title?
