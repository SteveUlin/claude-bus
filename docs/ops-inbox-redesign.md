# ops-inbox redesign — log-shaped, colorized, scannable

Author: elodin · For: comms / sulin · Status: proposal, no code yet.

## (a) what ops-inbox should be after redesign

A **colorized append-only log** of infrastructure signals and direct asides — visible at a glance in the cockpit, scrollable without ceremony, one line per event with timestamp, source, and a short body. The semantic shape stays the same (sulin's auto-memory framing — "infra signals + direct asides, not chatter") but the *display* stops looking like a mailbox dump and starts looking like a structured tail.

What it is NOT: a queue to be acked. Nothing reads ops-inbox-as-recipient; the broker writes to it, the human reads it, no agent consumes. Today it's typed `agent-inbox` because that's what auto-creates on the broker's escalation path — that's an implementation accident, not the intended shape.

## What lives in ops-inbox today

Examining a recent peek: ~30+ records, all `sender=broker, protocol=delivery-failure`, mostly recursive stale-epoch escalations from the cross-restart period. Each renders as a 7-line stanza (`=== id ===` + sender + protocol + ttl_ms + deliver_when + body label + body). The body itself is often a single line, sometimes nested ("delivery to ops exhausted (...): delivery to ops exhausted (...): ..."). One screen of the current viewer shows 4–5 records; a log-shape would show 30–40.

## (b) candidate shapes

### (1) Pure cosmetic — single-line tail viewer

Make `bus topic inbox ops` itself render log-style instead of stanza-style. Format per record:

```
HH:MM:SS [sender/protocol] body-first-line...
```

- Color: timestamp dim; `[sender/protocol]` bracket colored by severity heuristic (`delivery-failure` → red; `audit` → yellow; `auto-clear` → magenta; `text` / `note` → cyan).
- Multi-line bodies: take the first line, append `…` if more. Full body available via `bus msg body <id>`.
- Pros: shipping cost is small (~50 lines in `src/sub/sub_inbox.cpp`), no schema change, no migration. Big UX win — density goes 4× per screen.
- Cons: the per-record viewer (stanza format) is now hidden for ops; callers who want it use `bus msg peek inbox-ops` instead. Acceptable since `peek` already does that shape.

### (2) Semantic restructure — kind = append-log + new verb

Change `inbox-ops` from `agent-inbox` to `append-log` kind. Add a new verb `bus log ops` (or `bus tail ops`) for the log-shape viewer.

- Pros: types reflect intent. No one ever needs an "ack" semantics on ops; the kind should say so.
- Cons: requires a migration (the broker has hard-coded `inbox-ops` auto-creation as agent-inbox in `delivery::escalate`). New verb is more surface than necessary — the same display can hang off the existing inbox verb. The append-log kind today has no consumer side and almost no test coverage.
- Verdict: not worth the cost for this round. Re-evaluate if more append-log topics show up.

### (3) Multiplex — aggregate audit + ops + comms-asides into one view

Make the cockpit viewer read from {audit topic, inbox-ops, plus a new comms-aside protocol} and merge by timestamp. Color per source.

- Pros: one place to watch for everything infra-shaped.
- Cons: requires N-topic merge logic in the viewer; cursor semantics get messier (each source has its own cursor); the broker would need to stop double-writing audit + inbox-ops for the same event (today it writes both — see `delivery::escalate`). Significant invasiveness.
- Verdict: defer. Can come later if (1) doesn't carry enough signal.

### (4) Severity field

Add an explicit `severity` (INFO / WARN / ERR) to the wire format OR encode in `protocol` (e.g., `protocol="audit:INFO"`). Color by severity.

- Pros: cleaner color mapping than guessing from protocol name.
- Cons: protocol-encoding is a kludge; wire-format bump is a real change. The protocol field already has enough variance (`delivery-failure`, `audit`, `auto-clear`, `text`) that the severity heuristic in (1) maps well without it.
- Verdict: optional layer on top of (1). Don't ship in v1 — see how far the protocol-name heuristic gets.

## (c) Recommendation — ship (1), defer the rest

Tighten `bus topic inbox ops` to a log-shape colorized single-line view. Concrete spec for the kvothe handoff:

- Detect "log mode" when topic == `ops` (or a `--log` flag if generalization to other topics is wanted later).
- Read records from cursor as today; for each, emit one line:
  `<dim>HH:MM:SS</dim> [<color>sender/protocol</color>] <body-first-line>[…]`
- Color rule (heuristic on protocol):
  - `delivery-failure`, anything containing `error` / `fail` → red
  - `audit`, `auto-clear` → yellow (auto-clear could be magenta if visually crowded by yellow audits — judgment call)
  - everything else (text, note, info) → cyan
- Multi-line body: first line only, `…` suffix if the body has any subsequent line.
- Total record-line width should fit a typical pane (≤ 110 chars); truncate body to fit, `…` at the truncation point.
- Keep the existing inotify-tail loop — only the render function changes.

Implementation handoff: **kvothe** (per the comms-routing heuristic — viewers and dashboards are kvothe's territory; existing `sub_inbox.cpp` lives in the same render-state lib kvothe has been editing for monitor's FOCUS / PROJECT columns). Surface design choice (severity color map, body truncation width) in the commit message.

## Out of scope for this doc

- The "direct asides" producer side (a `bus msg note <text>` verb, or a comms slash for "drop a line into ops without bothering anyone"). Worth its own pass once the viewer is good. Sulin's framing implies this *should* exist; just not in this doc.
- Cleanup of the recursive stale-epoch escalations currently polluting ops. Already mitigated for new records by my own `escalate()` stamping fix (commit 8534557); old records expire as the cursor advances or can be dropped with `bus msg drop`.
- Severity field promotion to first-class — see (4); leave for a future round if the protocol heuristic isn't enough.
