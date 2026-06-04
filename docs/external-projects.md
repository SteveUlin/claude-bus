# Bus Sessions Over External Projects

Today the fleet layout hardcodes `cwd=/home/sulin/claude-bus` on every agent
pane. The bus is the codebase the bus runs on. If we want to point a fleet at
*another* codebase — tempura, nixos-config, whatever — every agent needs to
launch in that project's tree, with isolated working copies, while still
talking to the same broker.

This doc proposes the smallest set of new abstractions to make that work:
**sessions**, **agent kinds**, and a generator that produces the per-session
layout. The broker, the wire format, and the existing `bus` subcommands stay
unchanged.

## What changes, what doesn't

**Unchanged.** One broker per laptop, state at `/tmp/claude-bus`, topics in
the same registry, agent names in the same flat namespace. Hooks still resolve
absolute to `/home/sulin/claude-bus/settings/hooks/`. `bus msg send`, `bus msg mail`,
`bus msg slash`, the broker daemon — none of this needs to know that a target
project exists.

**New.** A *session* describes a fleet pointed at a target project. Sessions
declare per-agent **kinds** that decide cwd, workspace provisioning, and
claude-launch behaviour. A small generator renders a session into a zellij KDL
layout at launch time. Workspaces — per-agent isolated checkouts of the target
project — live under `~/<project>-workspaces/`.

## Session as the unit

A session file lives at `sessions/<name>.kdl`:

```kdl
session "tempura" {
    project "/home/sulin/tempura"

    agent "tempura-alice" kind="coder"
    agent "tempura-bob"   kind="coder"
}
```

Project sessions are pure coder fleets. The human's coordinator agent
(`comms`) lives in its own session. Names are
session-prefixed because the broker namespace is flat across sessions.

`bus session start tempura`:

1. Reads the session file.
2. Provisions a workspace for each `coder` agent at
   `~/tempura-workspaces/<agent>/` if absent.
3. Installs bus hooks into each workspace via `.claude/settings.local.json`.
4. Renders `layouts/.generated/tempura.kdl`.
5. `exec zellij --layout layouts/.generated/tempura.kdl`.

Idempotent: rerunning resumes existing workspaces and reattaches claude
sessions by UUID via the existing `agent-launch` logic.

## Agent kinds

The current fleet conflates *agent identity in the bus* with *agent runs
claude in the project tree*. Splitting that is what makes external projects
work.

| Kind          | cwd                          | Workspace?         | Process                          |
| ------------- | ---------------------------- | ------------------ | -------------------------------- |
| `coordinator` | `/home/sulin/claude-bus`     | no                 | `claude` via `agent-launch`      |
| `coder`       | `<workspace>`                | yes (per-agent)    | `claude` via `agent-launch`      |
| `observer`    | n/a                          | no                 | `bus topic inbox NAME` / `bus monitor` |

- **`coordinator`** is a claude pane with no workspace. Accepts a `role`
  augmentation. The canonical use is
  `kind=coordinator role=comms` for the human's comms helper, which lives
  in its own zellij session.
- **`coder`** is the role you actually scale: each one gets its own working
  copy so concurrent edits don't trample each other.
- **`observer`** isn't claude at all. It's a tab that runs a bus viewer.
  Used for ambient panes the human glances at — fleet monitor, broker
  daemon, the `inbox-ops` tail. The comms session is the primary user of
  this kind.

`agent-launch` grows two flags to match:

```
agent-launch --kind coordinator NAME
agent-launch --kind coder --workspace PATH NAME
# observer kind is launched directly by the layout — no agent-launch needed.
```

The kind drives cwd selection, decides whether to install
`settings.local.json`, and (for `observer`) skips claude entirely so the layout
can just run a viewer.

## Workspaces: jj-on-git, via the worktree workaround

tempura is git, claude-bus is jj, and the other targets will be a mix. We want
one mental model: every coder workspace is a **jj workspace**. Coders use
`jj` regardless of what the upstream uses. For git projects we use the known
jj+git colocation workaround until `jj workspace add --colocate` lands.

### The workaround

From [ipetkov on jj#8052][colocate-workaround]:

```sh
NAME=alice
ROOT=/home/sulin/tempura
WS=/home/sulin/tempura-workspaces/$NAME

cd "$ROOT"
git worktree add "$WS" --no-checkout --detach
mv "$WS" "${WS}.tmp"           # hide the .git file from jj
jj workspace add --name "$NAME" "$WS"
mv "${WS}.tmp/.git" "$WS/"     # restore the .git pointer alongside .jj
rmdir "${WS}.tmp"
(cd "$WS" && git reset .)      # clear the staged "everything" git sees
```

Each coder workspace ends up with both a `.jj/` and a `.git` pointer file. jj
manages history, git tooling (LSPs, build systems that probe `.git`) still
works inside the worktree.

**Caveat.** Colocated workspaces have a known rough edge: `git_head()` is
shared across workspaces, so `git diff` in a non-default workspace diffs
against the *main* worktree's HEAD, not the local one. For us this only bites
if a tool a coder runs inside the workspace shells out to `git diff` without
specifying revs — agents using `jj diff` are fine. Document it; revisit when
the [`jj workspace add --colocate` PR stack][colocate-prs] lands and we can
drop the shell hack.

### For jj projects

`jj workspace add --name NAME PATH` directly. No git wrangling. We branch by
upstream VCS at session-create time.

### Lifecycle

- `bus session start NAME` provisions any missing workspaces.
- `bus session stop NAME` — leave workspaces in place; agents may have
  uncommitted work.
- `bus session destroy NAME` — explicit cleanup; runs `jj workspace forget` and
  `git worktree remove` for each agent. Refuses if any workspace has uncommitted
  changes unless `--force`.

## Settings injection: layer via `settings.local.json`

Target projects often already have `.claude/settings.json` (tempura does) and
their own `settings.local.json`. We can't overwrite either. Instead, the
generator writes a fresh `.claude/settings.local.json` *inside each workspace*
containing only the bus hooks. Claude Code merges `settings.local.json` over
project settings, and hook arrays union by matcher, so the target's hooks and
the bus hooks both fire.

The workspace `.claude/` is a checkout of the target's tracked tree (worktrees
inherit it). Only `settings.local.json` is bus-owned, which is the same file
git/jj already gitignore in most setups. No symlinks, no copying, no mutating
the target repo.

Hook command paths stay absolute (`/home/sulin/claude-bus/settings/hooks/...`),
so the hooks always resolve to the bus repo regardless of where the workspace
lives.

## Layout generation

zellij KDL has no variable substitution, so we render. `bus session start`
materializes `layouts/.generated/<name>.kdl` from the session file. The
rendered file is just KDL — readable, debuggable, regenerated each launch.
`.generated/` is gitignored.

Per-agent panes look like:

```kdl
agent_tab name="alice" {
    pane size=1 borderless=true command="/home/sulin/claude-bus/bin/bus" {
        args "agent-bar" "alice"
    }
    pane name="alice"
         cwd="/home/sulin/tempura-workspaces/alice"
         command="/home/sulin/claude-bus/bin/agent-launch" {
        args "--kind" "coder" "--workspace" "/home/sulin/tempura-workspaces/alice" "alice"
    }
}
```

The ops tab and the broker pane are identical across sessions; only the agent
tabs change. The generator is a couple hundred lines of templating in
`src/bin/bus_session.cpp` or a sibling.

## One broker, many projects: topic naming

sulin's call: a single broker serves multiple concurrent sessions. Agent name
collisions become the real concern — two sessions can both want `alice`. Two
options:

1. **Project-prefix names in the layout, flat in the broker.** Agents launch
   as `tempura-alice`, `claude-bus-primary`, etc. The bus stays unchanged;
   collisions become a session-config lint problem ("name already in use by
   another running session"). Simplest. Mild ergonomic cost when typing
   `bus msg send tempura-alice "..."`.
2. **Broker-side topic namespacing.** Topics become `tempura/inbox-alice`,
   agent ids stay short. Bigger broker change (parser, registry, cursor
   layout). Defer until option 1's ergonomic cost actually bites.

Default to (1). Revisit if it gets annoying.

## What this isn't

- **Not a new daemon.** No session manager process. `bus session start` is a
  one-shot generator + exec. State of "is the session running" is "is its
  zellij layout still up".
- **Not a sandbox.** Coder workspaces are isolated checkouts, not network or
  filesystem sandboxes. Agents in a tempura workspace can still write outside
  it. Pair with the bypass-permissions + network restriction policy already
  in flight if isolation matters.
- **Not portable yet.** Paths assume `/home/sulin/`. Templating with `$HOME`
  is straightforward; do it when a second machine cares.

## Walkthrough: tempura and claude-bus side by side

A concrete day, assuming everything in this doc has
been built.

### Starting state

The comms session is the human's daily-driver zellij session, always
up:

```
$ zellij list-sessions
comms        created 3d ago   (current)
```

Inside: ops tab with the broker daemon, `bus monitor`, and an
`inbox-ops` tail; the comms agent in its own tab. The broker is the
one and only on the laptop; topics include `inbox-comms`,
`commands-comms`, `inbox-ops`.

### Bringing tempura up alongside

First-time setup is a session file:

```kdl
// sessions/tempura.kdl
session "tempura" {
    project "/home/sulin/tempura"
    agent "tempura-alice" kind="coder"
    agent "tempura-bob"   kind="coder"
}
```

No coordinator agent in the project session — comms handles that from
its own session. Launch:

```
$ bus session start tempura
[provision] tempura-alice → /home/sulin/tempura-workspaces/alice (new jj workspace via worktree workaround)
[provision] tempura-bob   → /home/sulin/tempura-workspaces/bob   (new jj workspace via worktree workaround)
[inject]    settings.local.json → both workspaces
[render]    layouts/.generated/tempura.kdl
[zellij]    starting session "tempura"
```

A new zellij session boots with just the two coder tabs. The comms
session is still running, detached.

```
$ zellij list-sessions
tempura      created 12s ago  (current)
comms        created 3d ago   (EXITED)         # detached, not exited
```

The comms `bus monitor` view sees all coders across sessions — because
monitor reads the broker, not the local layout.

### A morning of parallel work

The human's tab-switching pattern, mediated by comms:

- **In `comms` zellij session.** Type to the comms pane: "refactor
  `tempura/src/symbolic5/eval.h` — extract the AST helpers." comms
  drafts:

  > [comms] sulin's request: extract the AST-printing helpers from
  > `eval.h` into a new `ast_print.h`. Keep the constexpr boundary.

  comms shows the draft, awaits "yes", then `bus msg mail tempura-alice
  "..."`. Reports: queued, alice currently idle.

- **Bob in parallel.** "Also ask tempura-bob to scan the call sites in
  `symbolic5/` so we know the impact." comms drafts a second message,
  you approve, comms sends.

- **Spot-check.** `Ctrl+P d`, then `zellij attach tempura`. Now you
  stare at alice's pane to watch progress. No interaction needed; you
  just want eyes on it. Detach back to comms when done.

- **Reply lands.** alice mails comms when done:
  `bus msg mail comms "[tempura-alice] AST extracted, jj log -r @"`. The
  broker pushes the record into the comms pane; comms's
  `UserPromptSubmit` fires; comms surfaces: "alice finished.
  Spot-check the diff or move on?"

### Observability across both

The `bus monitor` view shows every agent across every session:

```
NAME              STATE       TOOL           AGE  MAIL  ATTACH
comms             IDLE        --              4s    0    on   (focused)
bast              WORKING     Bash            1s    0    off
kvothe            STARTING    --             12s    1    off  (mail from comms pending)
elodin            IDLE        --             45s    0    off
tempura-alice     WORKING     Edit            3s    0    off
tempura-bob       IDLE        --             20s    0    off
```

The session column is implicit in the name prefix. If the prefix
convention gets annoying, that's the signal to revisit broker-side
namespacing.

The `inbox-ops` tail in the comms session catches anything broker-level
— delivery failures, retry exhaustion, audit escalations. It stays
quiet on a clean day.

### Wind-down

End of day, alice and bob each commit their work in their workspaces
(jj handles parallel workspaces fine — one shared op log). The tempura
session shuts down:

```
$ zellij kill-session tempura
$ bus session destroy tempura       # optional: removes the workspaces
[destroy] tempura-alice workspace is clean → removing
[destroy] tempura-bob has uncommitted changes → refusing (rerun with --force)
```

The comms session keeps running. The broker has no idea anything
ended — it just notices that two recipients have no presence and
queues anything sent to them.

### What the walkthrough exercises

- Multiple zellij sessions sharing one broker and one comms helper.
- The comms session as infrastructure host (broker, monitor, ops
  tail); project sessions as pure coder fleets.
- jj workspaces over a git repo, via the worktree workaround.
- `settings.local.json` injection layering on tempura's own `.claude/`.
- The two-inbox split: comms reads `inbox-comms`, ops tail catches
  failures.

## Cutover plan

In dependency order; each step is independently mergeable:

1. **Generalize `agent-launch`.** Add `--kind` and `--workspace`. Existing
   `layouts/fleet.kdl` keeps working: each agent gets `--kind coordinator`,
   no workspace, identical behaviour to today.
2. **Workspace provisioner.** A new `bus session provision-workspace
   <project> <agent>` subcommand (or a shell helper) that runs the jj-on-git
   workaround or `jj workspace add` based on VCS detection. Idempotent.
3. **Settings injector.** Writes `settings.local.json` into a workspace given
   a target path. Trivial; one small C++ helper or a shell script.
4. **Session file + generator.** Parser for `sessions/*.kdl`, renderer for
   `layouts/.generated/*.kdl`, `bus session start NAME` driver.
5. **First real session.** `sessions/tempura.kdl`. Validate end-to-end:
   provision two coder workspaces, run a small task across them, confirm the
   broker happily routes between sessions if claude-bus's own fleet is still
   running.
6. **Lifecycle commands.** `bus session list / stop / destroy`.

Defer until needed: topic namespacing, fully portable paths, observer kinds
with non-claude commands (the layout can already run anything via `command=`).

## Open questions

- **Should `coordinator` agents stay in `/home/sulin/claude-bus`, or in the
  target project's root?** The session config lets either work. A
  coordinator in the target project sees the codebase directly but loses the
  bus repo as cwd; in the bus repo it's the inverse. Leaning bus repo —
  coordinators don't edit code, they edit the bus's view of work.
- **One session per zellij server, or many?** zellij is happy to host
  multiple sessions on one socket. With one broker, multi-session is fine.
  Just pick distinct zellij session names (`zellij -s tempura`,
  `zellij -s claude-bus`) so attach is unambiguous.
- **Should `bus spawn NAME` learn about kinds?** It currently assumes the
  bus repo as cwd. When a session is up, `bus spawn alice` should be able to
  resolve "alice belongs to session tempura" and launch in the right
  workspace. Implementable as a small session-registry file under
  `$CLAUDE_BUS_STATE/sessions/`.

## References

- [jj#8052 — Tracking issue for colocated workspaces][colocate-tracking]
- [ipetkov's worktree workaround comment][colocate-workaround]
- [PR stack #8642–#8647 implementing `jj workspace add --colocate`][colocate-prs]
- [jj docs: Working with GitHub][jj-github]

[colocate-tracking]: https://github.com/jj-vcs/jj/issues/8052
[colocate-workaround]: https://github.com/jj-vcs/jj/issues/8052#issuecomment-3704326177
[colocate-prs]: https://github.com/jj-vcs/jj/issues/8052#issuecomment-3764944926
[jj-github]: https://docs.jj-vcs.dev/latest/github/
