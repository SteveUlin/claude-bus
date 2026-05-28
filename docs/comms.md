# comms — the human's interface to the fleet

The fleet's old shape had a `primary` agent that ran in the claude-bus
repo, talked to the human, and sometimes did code work itself. The roles
muddled. When we started planning external projects
(`docs/external-projects.md`), the muddle showed up as "should every
project session have its own primary?" — and the answer was unsatisfying
either way.

This doc retires `primary` and replaces it with `comms`: a single agent
whose only job is to be the human's interface to the rest of the fleet.
Drafting, dispatching, surfacing replies, reading peer state. No
workspace, no code edits, no project allegiance.

## What comms is, what it isn't

comms is the **cockpit upgrade**, not a new layer of authority. The
human stays in charge — `docs/design-philosophies.md` calls this the
Cockpit philosophy. comms exists because the human is one person with
finite reading bandwidth, and the fleet generates more activity than one
person can monitor and message by hand.

Six core jobs, in order of how often they fire:

1. **Status** — "What's the fleet doing?" comms reads broker state,
   recent events, dumps a few panes that look interesting, returns a
   one-screen summary.
2. **Draft and dispatch** — "Tell alice to extract the AST helpers."
   comms pulls alice's recent activity for context, drafts a brief
   that builds on what alice already knows, awaits human approval,
   sends.
3. **Reply surfacing** — coders mail comms with status. The bus's
   `UserPromptSubmit` hook wakes comms; comms summarizes for the
   human along with relevant context.
4. **Multi-recipient dispatch** — "Break this across kvothe and
   elodin." One draft per recipient, one approval gate covering all
   of them, then send.
5. **Pulling replies** — "Did alice get back to me?" comms checks its
   inbox or dump-screens alice's pane.
6. **Mediation** — two coders need to coordinate; comms synthesizes
   both sides and proposes a resolution. Rarer.

What comms **doesn't** do:

- Edit project source. No workspace at all.
- Run builds, tests, or long-running work.
- Send to coders without human approval. Every dispatched message
  goes through "here's the draft" → "yes" → send.

The approval gate is non-negotiable in v1. The cost of a wrong message
hitting a coder mid-task is higher than the convenience of skipping
"yes." Revisit only if approval becomes a bottleneck in practice.

## The two-inbox split: `comms` and `ops`

The old shape had a single `inbox-human` that mixed real messages (a
coder reporting progress) with infrastructure noise (the broker
complaining that a delivery to `bast` failed after three retries). The
human had to read both and triage.

Split it:

- **`inbox-comms`** — real messages. Anything a coder mails meant for
  human attention lands here. comms reads it as its own agent inbox
  (the broker pushes records into the comms pane via the existing
  `agent-inbox` dispatch). comms is the renderer; it adds context,
  surfaces selectively, asks the human about ambiguous cases.
- **`inbox-ops`** — infrastructure traffic. Broker retry-exhaustion
  escalations, delivery-failure records, hook errors, audit summaries.
  Pure read-only display, tailed by an observer pane in the comms
  session. The human glances at it when something feels off.

Practical effect: the human's daily-driver attention sits on the comms
pane (the AI's summary). The ops tail is ambient awareness, not a
primary surface. The two channels never interleave.

This is one broker rename, not a new mechanism. The broker's
reliability code (currently mailing `inbox-human` after retry
exhaustion) retargets to `inbox-ops`. Both are `agent-inbox` kind
topics; the broker doesn't care that ops's "recipient" is a viewer
pane rather than a claude session.

## Where comms lives

A dedicated always-on zellij session named `comms`, hosting the
infrastructure pieces alongside the comms agent:

```
zellij sessions:
  comms            ← human's daily attach. Contains:
                     - comms agent pane (the AI)
                     - broker daemon (floating)
                     - bus monitor
                     - inbox-ops tail
  claude-bus       ← coders editing the bus codebase
  tempura          ← coders editing tempura
  (future projects)
```

The bus repo is a project like any other. Coders edit it in the
`claude-bus` session. Infrastructure — broker, monitor, comms, ops
tail — lives in the comms session, decoupled from any single
workload.

The human's daily attach is to `comms`. They detach and attach to
project sessions when they need to spot-check or take over a specific
agent; comms stays running.

## Agent discovery

Before drafting a message to alice, comms needs to know:

1. Does alice exist? (running, registered)
2. What is alice for? (kind, role, project, workspace)
3. Is alice available? (idle / working / stuck / has-pending-mail)
4. What has alice been doing recently?

The bus already covers (1), (3), (4) — `zellij action list-panes`,
`bus monitor`, `bus state`, `bus events`, `zellij action dump-screen`.
The gap is (2): identity metadata. Without it, comms can't phrase a
request in project-relative terms ("the file you were looking at
yesterday in `eval.h`") because it doesn't know which project alice
is pointed at.

### The agent registry

A small JSON file per live agent at
`$CLAUDE_BUS_STATE/agents/<name>.json`:

```json
{
  "name": "tempura-alice",
  "kind": "coder",
  "role": null,
  "session": "tempura",
  "project": "/home/sulin/tempura",
  "workspace": "/home/sulin/tempura-workspaces/alice",
  "pane_id": "terminal_7",
  "started_at": 1716774000
}
```

Hook lifecycle:

- `SessionStart` writes the file. The hook reads env vars seeded by
  `agent-launch` (`CLAUDE_BUS_AGENT_{KIND,ROLE,WORKSPACE,PROJECT,SESSION}`)
  plus the existing `CLAUDE_BUS_AGENT_ID`.
- `SessionEnd` removes it.
- The broker prunes opportunistically when its pane scan notices an
  entry whose pane no longer exists.

The registry is a side-effect, not a coordination primitive. Stale
entries are tolerable.

### `bus agents` and `bus introduce`

Two new subcommands:

```
bus agents                          # list all live agents
bus agents --json                   # for tooling
bus agents --kind coder             # filter by field
bus agents --session tempura
bus agents --role comms
bus agents NAME                     # one agent's card
```

```
bus introduce NAME
  → composite card: registry entry + bus state + last 30m of events,
    optionally a dump-screen tail with --deep.
```

`bus introduce` is the higher-level helper comms reaches for before
drafting. It bundles the calls comms would otherwise make individually.

### Why a registry, not "scan panes"

`zellij action list-panes` returns names and pane ids. It doesn't tell
you what an agent is *for*. The session config knows; the registry is
how that knowledge becomes visible to peers at runtime. Without it,
every agent would need to read the session KDL — coupling, brittle,
hard to keep in sync.

## Slash commands

Four shortcuts ship with v1, each a `.claude/commands/<name>.md` file.
comms reads natural language fine; the commands codify the
high-frequency motions so they're predictable.

- **`/status`** — fleet snapshot. Composes `bus agents`, `bus state`,
  `bus events --since 10m`, feeds comms a structured payload, comms
  produces the human summary.
- **`/peek <agent>`** — deep card on one agent. Wraps
  `bus introduce <agent>`.
- **`/draft <agent> <intent…>`** — jump straight to drafting. Runs
  `bus introduce` first, then drafts. Still awaits approval.
- **`/dispatch <plan>`** — multi-recipient. Breaks the plan into
  per-agent briefs, presents one combined approval gate, sends on
  "yes".

Typical flow: `/status` → `/peek alice` → `/draft alice "do X"` →
"yes".

## Communication patterns this enables

Each is a way of composing comms with the bus's existing verbs. No new
bus features required.

- **Direct (human ↔ comms)** — default. Human types intent; comms
  acts.
- **Brief-and-dispatch (comms → coder)** — the core US-2 pattern.
- **Reply surfacing (coder → comms → human)** — `[sender]`-prefixed
  mail to `inbox-comms`; comms surfaces with context.
- **Broadcast (comms → many)** — via `bus msg broadcast`.
- **Standup (comms → many → aggregate → human)** — comms broadcasts
  "status?", collects replies into `inbox-comms`, summarizes once.
- **Pipeline (comms → A → comms → B)** — comms tells A "mail me when
  done"; when A's reply lands, comms drafts B's task using A's
  output.
- **Mediation (A ↔ comms ↔ B)** — two coders out of sync; comms
  drafts a coordination message.

### Conventions

- comms→coder messages start with `[comms]`.
- coder→comms replies start with `[<sender>]`.
- comms never sends to an agent not in `bus agents`.
- comms never edits files; if asked, it delegates to a coder.

## Walkthrough: a morning at the comms pane

The human attaches to `comms` and types `/status`. comms reports:

> bast finished phase-4h cleanup (5m ago, idle). kvothe is mid-edit
> on `src/session.cpp` (12s ago). elodin has been on the same Read
> for 90s — looks stuck on macro expansion in `agent_status.h`.
> tempura session is quiet; alice's last activity was 2h ago.

The human follows up: "give alice the AST extraction task we drafted
yesterday." comms `/peek tempura-alice` for context, drafts:

> [comms] sulin's request: extract the AST-printing helpers from
> `eval.h` into a new `ast_print.h`. You explored this file yesterday
> (transcript 14:32: "the print helpers are the obvious slice"). Keep
> the constexpr boundary you flagged then.

The human reads, says "yes." comms sends, reports: "queued to
tempura-alice; she's mid-Bash, lands at next prompt."

An hour later alice mails comms back:
`bus msg mail comms "[tempura-alice] AST extracted, jj log -r @, ready
for review"`. The broker pushes the record into the comms pane.
comms's `UserPromptSubmit` fires and surfaces:

> alice: AST helpers extracted, ready for review. Want me to spot-check
> the diff or move to the next slice?

Meanwhile a separate broker escalation fires — `bast` was sent a slash
command but didn't ACK after three retries. The broker appends the
failure to `audit` and mails `inbox-ops`. The ops tail pane in the
comms session shows the record. comms's pane does not. The human's
glance catches the ops tail, decides whether to detach and attach to
the bus session to check on bast.

## Composition with external-projects

`docs/external-projects.md` defines the session abstraction and the
three agent kinds (`coordinator`, `coder`, `observer`). comms fits in
cleanly:

- **comms is a `coordinator`** with `role=comms`. No workspace, cwd
  irrelevant. The session generator passes `--role comms` to
  `agent-launch`, which appends `roles/comms.md` to the system prompt.
- **Project sessions are pure coder fleets.** The earlier walkthrough
  in `external-projects.md` shows a `tempura-primary` — replace that
  with just `tempura-alice` / `tempura-bob`. Coordination flows
  through the comms session, not a per-project primary.
- **Cross-session naming stays flat.** comms addresses any coder by
  bare name (`tempura-alice`, `bast`) regardless of which session
  hosts them. One broker, one namespace.

The `coordinator` kind narrows in scope: "claude pane, no workspace,
accepts a `role`." `kind=coordinator role=comms` is the canonical use
and, for now, the only one.