# comms-UI: one-sentence-per-agent activity card

sulin's clarification: *"I want it to be a thing I can reference to
know, ok, agent x is working on testing now, or agent y is running a
sim."* What's wanted is a **reference card, not a log**, and not a
stat-block either. One sentence per agent that reads as a sentence:
`<name> is <doing X>`. Scannable at a glance. No transcript.

## What's missing today

- `bus log` reads agent-by-time. Useful for debugging dispatch, wrong
  for the cockpit.
- `bus monitor` is one-line-per-agent but tabular — eight columns
  drowning the one signal that matters.
- Neither surface pins *"agent X needs you to answer Y"*. The signal
  exists (state = NEEDS_INPUT) but isn't called out.

The gap: a verb that answers "who's working on what" in one sentence
per agent.

## Proposed surface — `bus deck`

Per-agent line. Two zones: ⚠ NEEDS YOU (pinned) then ✓ FLEET
(WORKING → IDLE, recency-sorted). Sample frame:

```
🚌 claude-bus · 4 agents · 1 needs you

⚠  elodin    asking: "should the auto-clear gate on epoch shift?"

✓  kvothe    working on  design comms UI                        (1m)
   auri      working on  hub coordination                       (60s)
   bast      idle                                               (5m)
```

One sentence per agent. The action verb is built from state + title:

| State          | Sentence                                              |
| -------------- | ----------------------------------------------------- |
| WORKING        | `<name> working on <title>`                           |
| NEEDS_INPUT    | `<name> asking: "<permission_prompt first line>"`     |
| IDLE           | `<name> idle`                                         |
| HAS_MAIL       | `<name> has N unread`                                 |
| STARTING       | `<name> starting`                                     |
| STUCK          | `<name> stuck (last: <last_event>)`                   |
| GONE / ENDED   | hidden                                                |

Title source priority: `$STATE/title/<agent>` → focus file → recent
`activeForm` from events tail. Same chain monitor's TITLE column
already uses.

## Sorting + filtering

- NEEDS_INPUT pinned at top, sorted oldest-first (the longest pending
  ask is loudest).
- WORKING next, recency-sorted by `age_ms` from `bus state`.
- IDLE / STARTING below, dim.
- GONE / ENDED hidden unconditionally. (Use `bus state --all` for
  tombstones.)
- The NEEDS YOU zone disappears entirely when no agent is in
  NEEDS_INPUT — no empty header.

## Where it lives

Replace `bus-log` in `layouts/fleet.kdl` (right column above
`jj-log`) with `bus deck`. `bus log` keeps its verb but isn't pinned
to the cockpit. `bus monitor` keeps its row for the rough fleet shape
(broker alive? attach state? mail counts?) — `bus deck` answers
"what's going on" in english.

## Out of scope (v1)

- Drill-down / interactive. Read-only render, 1 Hz.
- Transcript or thread view. One sentence per agent — debugging
  belongs in `bus log`.
- Cross-agent flow ("auri dispatched to kvothe"). Hub-coordination is
  auri's surface, not the cockpit.

## Open for sulin / comms

- `bus deck` or another name? (`bus brief`, `bus desk`, `bus board`)
- Should `WORKING` show the age (`(60s)`) inline, or only on STUCK /
  IDLE where it actually signals?
- Refresh cadence: 1 Hz tick (matches monitor) or event-driven via
  inotify on events.jsonl + focus/title?
