# pane-read audit — monitor pipeline (kvothe lane)

Author: kvothe · 2026-06-02 · For: sulin's first-class "stop pane-reading"
goal. Step 2 of the monitor-truth increment (step 1 = the statusline wrapper,
which moved ctx/window/effort off any pane-read onto a file — DONE,
live-verified).

Durable anchor (survives /clear). Builds on
[docs/monitor-truth.md](monitor-truth.md). See [[stop_pane_reading]] memory.

## Goal

Map every place the **monitor pipeline** (the surfaces sulin *sees* —
`bus monitor`, `agent-bar`, `log`, `state`, `agents`, `inbox`, `events`)
still depends on zellij pane scraping (`dump-screen` / `list-panes`) for
state, then make pane-reading the FALLBACK, not the primary. Pane-reads are
costly (a subprocess + a full screen dump per agent per poll) and fragile
(focus-dependent, racy, lie under scroll/lock/modal).

## Method

Grepped all viewer surfaces for `dump-screen` / `list-panes` /
`readPaneState` / `paneState`, then traced the broker `op:state` RPC (the
viewers' shared data feed) to its field sources.

## Finding 1 — the viewers are ALREADY pane-read-clean

**No viewer surface calls `dump-screen` / `list-panes` / `readPaneState`
directly.** Every column reads from the broker `op:state` RPC or a `$STATE`
file:

| Surface | Sources | Direct pane-read? |
| --- | --- | --- |
| `sub_monitor` | `op:state` RPC; `$STATE/{statusline,title,agents,triggers,status}` files | no |
| `sub_agent_bar` | `op:state` RPC | no |
| `sub_state` | `op:state` RPC | no |
| `sub_agents` | `op:state` RPC + `$STATE/agents/*.json` | no |
| `sub_log` | `events.jsonl`; `$STATE/{title,focus}` files | no |
| `sub_events` | `events.jsonl` | no |
| `sub_inbox` | topic logs | no |

(`sub_pane` / `bus pane-state` IS a direct dump-screen, but it is the
explicit debug introspection tool — pane-reading is its *purpose*, not an
incidental dependency. Out of scope.)

## Finding 2 — the one remaining dependence is broker-side, per-poll

The viewers inherit one pane-read transitively. The broker's `state` RPC
handler calls **`paneState(name)` once per agent, per request**
(`src/broker.cpp:576`) — a `zellij action dump-screen` per agent. The
monitor polls at ~1 Hz, so this is **N dump-screen subprocesses/sec** for an
N-agent fleet, on the broker's hot path. `paneState` →
`readPaneState` (`src/agent_status.h:206`) → `dump-screen` (`src/pane.cpp:652`);
agent discovery uses `list-panes` (`src/pane.cpp:470`).

That scan populates these `op:state` fields: `pane.ok` → `pane_exists`,
`pane.mode` → `mode`, `pane.buffer` → `buffer`, `pane.bypass_perms`, and it
feeds `computeState` / `computeAxes` (the mode-dependent slices —
NeedsInput / Compacting / BootStuck).

## Finding 3 — the monitor RENDERS almost none of the pane-scraped fields

The decisive observation. Of the pane-derived fields, the monitor displays:

| Pane-derived field | Rendered by monitor? | Already has a non-pane source? |
| --- | --- | --- |
| `attached` | yes (attach dot) | **YES — `hasPresenceFile()`, a `$STATE/presence/` file** (broker.cpp:637). Already file-based; not from the scan. |
| `pane_exists` | yes (GONE/ENDED filter) | no — needs the scan or a lifecycle hook |
| STATE label (mode slice) | yes (NeedsInput/Compacting/…) | PARTIAL — Compacting/NeedsInput/BootStuck also derive from `events.jsonl` payload (source, tool_name); the pane `mode` is only a supplement |
| `buffer` (draft) | **no** — `(void)has_draft` (sub_monitor.cpp:456) | n/a — fetched but unused |
| `mode` (raw) | no | n/a |
| `bypass_perms` | no | n/a |

So the monitor's ACTUAL pane-derived needs reduce to **`pane_exists`
(liveness/GONE)** and **the mode supplement to the STATE label**. `attached`
is already file-sourced; `buffer`/`mode`/`bypass_perms` are fetched but never
shown. The broker pays for a full screen dump per agent per poll to surface
two things, one of which an event/hook feed could supply.

## Routing — who owns the migration

- **Monitor side (me): DONE.** No viewer change is needed to "stop
  pane-reading" on the surfaces sulin sees — they're already RPC/file. The
  statusline wrapper closed the ctx/window/effort gap; `attached` is already
  presence-file-based. My side of the goal is met.
- **Broker side (elodin): the live work.** The per-poll `paneState(name)` at
  broker.cpp:576 is the one pane-read in the pipeline, and it's broker
  internals. Proposed migrations, for elodin to weigh:
  1. **Drop the unused fields from the scan path** — the broker fetches
     `buffer`/`mode`/`bypass_perms` that no viewer renders. If nothing else
     needs them, the scan can be skipped or made lazy. (Delivery readiness
     may need `mode`; that's elodin's call — it's the broker-side
     pane-read consumer.)
  2. **Feed `pane_exists` from lifecycle, not a scan** — a SessionStart/Stop
     hook (or the registry + a heartbeat) can mark liveness in a `$STATE`
     file; the broker reads the file, falls back to a scan only when stale.
     This is the same shape the [[gone_axis_debounce]] note wants (GONE on a
     debounced signal, not the first missed query).
  3. **Mode-to-file** — if the STATE-label mode supplement is worth keeping,
     a hook can stamp the current mode to `$STATE` on transition, so the
     broker reads a file instead of dumping the screen.

## Status / next

1. ~~audit the monitor pipeline for pane-reads~~ — DONE (this doc).
2. Monitor side: nothing to migrate — already RPC/file. The "stop
   pane-reading" goal on sulin-facing surfaces is **met** on my lane.
3. Hand findings 2 + 3 to **elodin** (broker-side scan is the one remaining
   pane-read); coordinate the cross-cut. The highest-leverage single change
   is dropping the unused `buffer`/`mode`/`bypass_perms` fetch and/or
   feeding `pane_exists` from a lifecycle file.
