# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

`claude-bus` is a small, opinionated harness for running **multi-agent Claude Code sessions inside zellij**. One human, many agents, one terminal. The user can attach to any agent's pane to read its scrollback, take over the keyboard, or hand control back.

The architecture below is the target shape — where code exists it follows the design; where it doesn't, the design is the contract for what to add.

## Architecture

Four layers, mapped to the top-level directories:

- **`layouts/`** — zellij KDL layouts. A layout defines the pane topology for a session: typically one "bus" pane (orchestrator / status) plus N agent panes, each launching `claude` (or `claude -p`) with a role-specific prompt. Layouts are the unit a user invokes when they want to start a multi-agent session.
- **`bin/`** — `bin/bus` is the unified C++ entry point for everything (broker, viewers, pane helpers, producers, consumers, lifecycle). `bin/agent-launch` is the only remaining shell helper, used by `bus spawn` to resolve a stable session UUID before `exec claude`. Built by CMake from `src/` via `nix develop` + `cmake --build build`. Compiled binary is gitignored; the source-of-truth lives in `src/`.
- **`settings/`** — hook scripts shared by every agent in a session (e.g. `settings/hooks/log-event.sh`), plus the canonical Claude Code project config at `settings/claude-settings.json` (tracked, public). Claude Code only auto-loads `.claude/settings.json`, so `bin/agent-launch` symlinks each workspace's `.claude/settings.json` → `settings/claude-settings.json` on every launch. That path is gitignored, so jj never materializes divergent per-workspace copies and a config edit applies fleet-wide with no per-workspace commit + sync. **Edit `settings/claude-settings.json`, never `.claude/settings.json`** (it's a symlink). We tried `CLAUDE_CONFIG_DIR=settings` so the bus would have a fully isolated config; in practice it forced a fresh login every session, so we use the project-level layering instead.
- **`coordination/`** — library of patterns that layer on top of the bus (queue, blackboard, maildir, etc.). Each subdirectory is a self-contained pattern with its own README, agent-prompt augmentation, hooks, scripts, and initial state. The baseline `coordination/nothing/` documents the bus's default no-coordination shape. See `coordination/README.md` for the contract.

The "bus" metaphor is literal: agents are independent processes that observe a shared event stream (hook output) and can be addressed individually by the user via zellij pane focus.

## Observability

Two complementary channels, both first-class:

1. **Live scrollback** — every agent runs in its own zellij pane. Switching panes/tabs *is* the primary debugger. Design layouts so the panes a user most often wants to inspect are reachable without hunting.
2. **Event log** — Claude Code hooks (`PreToolUse`, `PostToolUse`, `Stop`, `UserPromptSubmit`, etc.) in `settings/hooks/` emit structured JSONL to a shared file or socket. The bus pane tails it. This is the cross-agent view that scrollback can't give you.

When adding a feature, ask: does it need the per-agent view, the cross-agent view, or both? Wire it accordingly.

## User intervention model

For now: **the user attaches to an agent's pane and types directly at the running `claude` session.** No IPC side-channel, no pause/resume protocol. Keep this simple until it stops being enough.

If a future feature needs out-of-band steering (inject a message without stealing the pane, pause an agent, hand off a session), prefer the smallest mechanism — a named pipe per agent, or Claude Code's session-resume — over a new daemon.

## Agent-to-agent comms (the bus)

If you're an agent running inside a bus pane, your `$CLAUDE_BUS_AGENT_ID` is set. You can message peer agents in the session by running:

```
bin/bus msg send <peer-name> "<message>"
```

The message lands in the peer's prompt buffer and submits. The peer receives it as a fresh user turn, indistinguishable from a human-typed message.

- **Discover peers.** `zellij action list-panes` shows every pane and its title. Bus agents have titles set by the launching layout (e.g., `alice`, `bob`). `bus pane-id NAME` resolves a name to its `terminal_N` id; exits non-zero if absent.
- **No reply channel.** The bus is fire-and-forget. To see how a peer responded, dump their screen: `zellij action dump-screen --pane-id "$(bus pane-id NAME)"`. To get a reply routed back to you, either the human relays it or the peer sends back through the bus.
- **Sign your messages** when the recipient needs to know who's talking. Convention: lead with `[your-name]`. Example: `bin/bus msg send bob "[alice] need your take on the cache invalidation idea"`.
- **One human gate.** The bus is the same bus the human uses. Sending to a peer doesn't bypass them — they're watching every pane and can interject.

### The broker (async queued delivery)

`bus msg send` writes into a pane's TUI buffer immediately — fine for urgent
direct messages, but it contends with the human keyboard. For queued / async /
typed delivery, go through the **broker daemon** (`bus broker run`), the bus's
single source of truth: it owns the topic registry, append-only topic logs,
per-consumer cursors, the 250 ms delivery loop, and the retry / ack / audit /
`inbox-human` escalation path. Hooks only emit state events to `events.jsonl`;
the broker decides when to push.

The full spec — topic kinds (`agent-inbox`, `tui-commands`, `work-queue`,
`pubsub`, `blackboard`, `append-log`), cursor / ACK semantics, the
`[bus-attach]` presence sentinel, reliability, and the broker **launch
contract** (must be a direct zellij child; never `nohup`/`setsid`/`disown` it)
— is in [docs/broker-spec.md](docs/broker-spec.md). Read it before touching
delivery or restarting the broker.

## Conventions

- **jj, not git.** Use `jj status`, `jj log`, `jj diff`, `jj describe`, `jj new`. Commit descriptions: imperative short summaries optimized for `jj log` readability, no conventional-commits prefixes.
- **C++23 for the bus internals.** New tools land as C++23 in `src/`, built by CMake into `bin/`. Flat `bus::` namespace. The reproducible dev env is `flake.nix` + `.envrc` (direnv); `nix develop` drops you in if direnv isn't enabled. Existing shell scripts will be rewritten as time permits.
- **One ruleset per session.** Don't let agents drift into per-agent `settings.json` overrides unless a role genuinely requires it — the shared `settings/` is the point.
- **Layouts are the API.** A new use case usually means a new layout in `layouts/`, not new flags on existing scripts.

## Code changes

Four principles for any edit here. Apply with judgment — trivial fixes don't need ceremony.

- **Think before coding.** State your assumptions explicitly. If multiple interpretations exist, surface them — don't pick silently. If a simpler approach exists, say so. If something is unclear, stop and ask.
- **Simplicity first.** Minimum code that solves the problem. No features beyond what was asked, no abstractions for single-use code, no error handling for impossible scenarios. If 200 lines could be 50, rewrite.
- **Surgical changes.** Touch only what you must. Don't "improve" adjacent code, refactor working code, or shift unrelated style. Remove imports/variables that *your* change orphans; don't delete pre-existing dead code without asking. Every changed line should trace to the request.
- **Goal-driven execution.** Translate vague tasks into verifiable goals: "Add validation" → write tests for invalid inputs, then make them pass. "Fix the bug" → write a test that reproduces it, then make it pass. Strong success criteria let you loop independently.

## Commands

Everything runs through the unified `bin/bus` binary; `bus help` lists every
subcommand. The full annotated catalog — lifecycle, topic registry,
produce/consume, direct/low-level pane writes, viewers — is in
[docs/bus-commands.md](docs/bus-commands.md). The broker daemon (`bus broker
run`) owns all topic state and drives delivery; CLI tools talk to it via
JSON-RPC on `$STATE/broker.sock`.

Start a bus session (focus shifts to the new tab):

- `zellij --layout layouts/fleet.kdl` — the working multi-agent layout: ops + primary + the named agent fleet (bast / kvothe / elodin). Each agent tab calls `bin/agent-launch NAME`, which resumes the agent's prior session by stable UUID so the fleet picks up where it left off after a host or zellij restart.
- `zellij --layout layouts/demo.kdl` — two bash panes; cheap for iterating on the bus itself.
- `zellij --layout layouts/two-claudes.kdl` — two claude panes with `--dangerously-skip-permissions`.

Add a layout to a running session without taking focus: pre-declare tabs in the startup layout, then push panes in with `zellij action new-pane --tab-id N -- <cmd>`. `new-tab --layout` always steals focus (no flag to suppress).

No build, lint, or test yet. Add them to this section as they land.
