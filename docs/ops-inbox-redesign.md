# ops-inbox redesign — events.jsonl-derived lifecycle log

Author: elodin · For: comms / sulin · Status: proposal, no code yet.

(Revised after sulin's clarification: ops-inbox is NOT an agent→sulin channel. Replace its viewer with a colorized lifecycle log derived from events.jsonl.)

## (a) what the cockpit pane should show after redesign

A **scannable colorized event log** showing fleet lifecycle transitions, one line per event. Three shapes the human cares about:

- `agent X started working on Y`
- `agent X finished with Y`
- `agent X needs user input`

Plus, folded in alongside, the infrastructure events the broker emits (delivery-failure escalation, auto-clear, drop). Same line shape, different color band.

The pane that today reads `bus topic inbox ops` becomes a new view — call it `bus log` for now — that tails `events.jsonl` and renders these transitions in real time.

The `inbox-ops` topic itself can stay (the broker still writes delivery-failure escalations there as a per-record audit trail), but it stops being what the cockpit shows. Worth eventually deprecating in favor of writing only to the existing `audit` topic, but that's a tidying pass — not blocking this redesign.

## What we have to read from

Two sources, already populated:

- **`events.jsonl`** — hook events keyed by agent. Maps to transitions:
  - `SessionStart` → "X started" (with `source` distinguishing startup / resume / compact)
  - `UserPromptSubmit` → "X received" (with `payload.prompt` first line)
  - `PreToolUse:TodoWrite` / `TaskCreate` / `TaskUpdate` (status→in_progress) → "X working on Y" (Y from `activeForm`)
  - `Stop` → "X finished" (Y resolved from `payload.last_assistant_message` first line or the focus file at event time)
  - `Notification` with `notification_type=idle_prompt` → "X idle"
  - `Notification` with `notification_type=permission_prompt` → "X needs input"
  - `SessionEnd` → "X ended" (with `reason` for clear / compact)

- **`$STATE/focus/<agent>`** — already maintained by `settings/hooks/focus-write.sh` (TodoWrite / TaskCreate / TaskUpdate hooks, kvothe just shipped this). Holds the current in-progress task's `activeForm`. Use as the "Y" when a transition doesn't carry one inline.

- **`$STATE/title/<agent>`** — sulin's per-mail title (`bus msg mail --title`, also kvothe). Higher priority than focus when set; falls back to focus otherwise.

## (b) candidate shapes

### (1) Replace the viewer in place; keep the topic

Make a new `bus log` verb (or `bus topic inbox ops --log` to keep the path stable for the fleet.kdl pane). It tails `events.jsonl` and the `audit` topic (broker emissions), merges by timestamp, renders one line per relevant transition:

```
HH:MM:SS  <agent>  ▸ started: implementing dispatch-tui
HH:MM:SS  <agent>  ◇ working: writing src/dispatch.cpp
HH:MM:SS  <agent>  ✓ finished: dispatch-tui ready for review
HH:MM:SS  <agent>  ❓ needs input
HH:MM:SS  broker   ⚠ delivery to bast exhausted: …
HH:MM:SS  broker   🗜 auto-clear kvothe (idle 12m)
```

Color: agent-color (already established per `bus agentColor`) for the agent name; severity glyph + body colored by transition kind (start=cyan, working=blue, finished=green dim, needs-input=yellow, broker-error=red, broker-info=magenta).

Pros: one viewer change, no schema change. Reads sources that already exist. The inbox-ops topic stays for audit-as-data; the viewer just stops being its renderer. Sulin gets the lifecycle log immediately.

Cons: events.jsonl is per-agent-event firehose — needs trimming. Many of its events (every PreToolUse:Bash, every PostToolUse:Read) are noise. Filter to the lifecycle subset above; ignore others.

### (2) Multiplex into a virtual topic

Promote the lifecycle log to a real "synthesized" topic the broker maintains. The broker would translate events.jsonl into a curated topic-log internally, and the viewer reads that topic as it reads any other.

Pros: separates concerns — viewer becomes dumb, the curation logic is server-side.

Cons: doubles writes (every interesting event ends up in two places), introduces a new topic kind ("synthesized" or "derived"), broker has to keep its synthesis in lockstep with the hook event stream. Significantly more surface for marginal cleanliness. **Defer.**

### (3) Topic-side restructure — `inbox-ops` → kind=append-log + multi-source viewer

(Carried over from v1 of this doc for context.) Touches broker code, requires migration, and now mostly orthogonal to sulin's actual ask: a *display* problem, not a *typing* problem. **Drop.**

## (c) Recommendation — ship (1), defer the rest

Concrete spec for the kvothe handoff:

1. New verb `bus log [--since DUR] [--agent NAME]` reads `events.jsonl` from EOF backwards by `--since` (default 5m), then live-tails forward. Optionally interleaves the `audit` topic.
2. Render one line per LIFECYCLE event only. The filter set:
   - `UserPromptSubmit`, `Stop`, `Notification` (idle_prompt / permission_prompt)
   - `SessionStart`, `SessionEnd`
   - `PreToolUse` only when `tool_name in {TodoWrite, TaskCreate, TaskUpdate}` AND the call corresponds to a status → in_progress transition. (The `focus/<agent>` file is already populated by the hook; reading the file at render time avoids re-parsing tool input.)
3. "Y" resolution priority per agent: `$STATE/title/<agent>` (if set) > `$STATE/focus/<agent>` > the event's inline payload (`prompt` / `last_assistant_message` first line) > empty.
4. Format: `HH:MM:SS  <agent>  <glyph> <kind>: <body>`. Glyph + kind chosen from the table above; agent rendered in its existing per-agent color.
5. Fleet-pane wiring: update `layouts/fleet.kdl`'s ops-inbox slot from `bus topic inbox ops` to `bus log` (keep the bash restart wrapper).
6. Test by enqueueing to `events.jsonl` directly (sub_events already does this for tests) and confirming each transition renders.

Implementation handoff: **kvothe** (viewer territory; already owns `sub_monitor` / `sub_inbox` / `sub_events` and the focus/title file conventions). Surface design choices in the commit:
- which transitions get a glyph and which fold (verdict: ship the 6 in the filter, ignore the rest)
- whether the audit topic is merged into the view from v1 or v2

## Out of scope

- Removing the broker's writes to the `inbox-ops` topic. Worth doing once the viewer no longer reads it; not blocking this doc.
- The `bus msg note` direct-asides channel from v1 of this doc — sulin's clarification explicitly drops this; the lifecycle log replaces it.
- Persistence of the lifecycle log itself. `events.jsonl` is already on disk; the viewer just renders it. If we ever want a curated structured log, that's option (2) above and a separate decision.
