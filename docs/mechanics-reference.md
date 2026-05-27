# Mechanics Reference: zellij + Claude Code for claude-bus

Field guide assembled from research, kept in-tree so we can revisit it as we build. Each section is mechanism → API → gotchas. Sources cited inline; full link list at the bottom.

---

## 1. The `zellij action` Surface (Your IPC Layer)

`zellij action` is a thin client that connects to a running zellij session and sends it a serialized message. The session is resolved in this order: explicit `--session NAME`, the `$ZELLIJ_SESSION_NAME` env var (set in every pane zellij spawns), or — if outside zellij entirely — the most recently used session. A hook script running *inside* a pane picks up the right session automatically; a cron job will not.

### Subcommands you actually need

- **`write BYTES…`** — Raw bytes to a pane's PTY. Args are integers, e.g. `zellij action write 27 91 65` is ESC `[` `A` (up-arrow). Only way to inject true control characters.
- **`write-chars STRING`** — UTF-8 text. `\n` is a literal newline character (0x0A). For TUIs that distinguish "newline" from "Enter key event," this is *not* the same as pressing Enter.
- **`send-keys --pane-id ID "Key" "Key"…`** — Human-readable key names: `"Enter"`, `"Ctrl c"`, `"Alt Shift b"`, `"F1"`, `"Esc"`. Translates to the appropriate byte sequence (CR for Enter, 0x03 for Ctrl-C). Prefer for "act like I pressed the key."
- **`paste --pane-id ID TEXT`** — Wraps text in bracketed-paste escape sequences (`ESC[200~` … `ESC[201~`). Faster than write-chars for multi-line, but has known bugs (see §3).
- **`focus-pane-with-id ID [--floating]`** — IDs look like `terminal_3` or bare integers (treated as terminal panes).
- **`new-pane [--direction down|right] [--floating] [--in-place] [--cwd PATH] [--name NAME] [--tab-id N] [--x N --y N --width N --height N] [--pinned] [-- CMD…]`** — Returns the new pane's ID on stdout.
- **`new-tab [--layout PATH] [--layout-string KDL] [--cwd PATH] [--name NAME]`** — Returns the new tab's ID.
- **`go-to-tab N`** / **`go-to-tab-name NAME [--create]`** — `--create` makes idempotent scripts trivial.
- **`move-focus left|right|up|down`** — Relative; ambiguous in multi-client scenarios, prefer `focus-pane-with-id` in scripts.
- **`dump-screen [--path PATH] [--full] [--pane-id ID] [--ansi]`** — Observability primitive; see §4.
- **`edit-scrollback [--pane-id ID] [--ansi]`** — Opens in `$EDITOR`. For humans, not scripts.
- **`query-tab-names`** — Tab names, one per line.
- **`list-clients`** — Attached clients with focused pane ID and program running.
- **`list-panes [--json]`** — Enumerate panes by ID, title, command. Runtime discovery loop.

### Pane IDs

A pane ID is `(kind, integer)` where kind is `terminal` or `plugin`. Inside any pane zellij spawned, `$ZELLIJ_PANE_ID` exposes the integer. IDs are stable for the pane's lifetime within a session, including across detach/reattach. They are NOT stable across new sessions or `zellij kill-all-sessions`.

Implication: persist the layout's `name` attribute as the canonical handle and resolve to ID at runtime via `list-panes --json`.

---

## 2. KDL Layouts — The Declarative Spawning Surface

```kdl
layout {
    tab name="orchestrator" {
        pane name="boss" command="claude" {
            args "--resume" "boss-session"
            cwd "/home/sulin/claude-bus"
            start_suspended false
        }
        pane split_direction="vertical" {
            pane name="worker-1" command="claude" hold_on_close=true
            pane name="worker-2" command="claude" hold_on_close=true
        }
    }
    floating_panes {
        pane name="event-log" command="tail" {
            args "-f" "/tmp/claude-bus/events.jsonl"
            x "60%"; y "5%"; width "38%"; height "40%"
        }
    }
}
```

Key `pane` attributes:

- **`command`** + **`args`** — what to run. Omitted = shell.
- **`cwd`** — resolved relative to the layout's `cwd` if relative.
- **`start_suspended true`** — pane created, command waits for Enter. Good for "pause for human to read before launching."
- **`hold_on_close true`** — pane stays open after command exits. Critical for agents: if `claude` exits, you want the last screen, not vanish.
- **`name "..."`** — pane title; the handle wrappers select on (`title:worker-1`).

`pane_template` factors out repetition:

```kdl
pane_template name="agent" command="claude" hold_on_close=true {
    args "--resume" "$AGENT_ID"
}
layout {
    tab name="agents" {
        agent name="planner"
        agent name="builder"
    }
}
```

`swap_tiled_layout` / `swap_floating_layout` auto-apply alternative arrangements when pane count crosses thresholds. Useful for fluctuating agent count.

**Tiled vs floating rule of thumb:** agents in tiled panes (persistent screen real estate, keyboard-navigable), utilities in floating panes (event log tail, status dashboard — toggle into view).

From a script always use `zellij action new-tab --layout PATH`. The bare `zellij --layout` form is for humans at a prompt and can accidentally start a new session if the env is wrong.

---

## 3. The `claude` CLI Input Model (the gotcha section)

Claude interactive mode uses [Ink](https://github.com/vadimdemedes/ink) (React for CLIs). It does NOT behave like a normal line-buffered REPL.

### Submit-key gotcha

`zellij action write-chars --pane-id 5 "hello\n"` writes literal bytes including newline (0x0A). Ink's input component expects **CR (0x0D)** as the submit key. Result: text with a newline sitting in the input box, unsubmitted. Single most common failure mode. [Issue #15553](https://github.com/anthropics/claude-code/issues/15553) documents this.

**The fix:**

```bash
zellij action write-chars --pane-id "$PANE" "hello from another agent"
zellij action send-keys --pane-id "$PANE" "Enter"
```

`send-keys "Enter"` emits CR, which Ink recognizes as submit.

### Bracketed paste — useful and dangerous

`paste` wraps text in `ESC[200~ … ESC[201~`. Claude supports bracketed paste for multi-line input, but reproducible bugs:

- [#3134](https://github.com/anthropics/claude-code/issues/3134) — Claude leaves bracketed-paste mode on after exit, parent shell sees pastes prefixed `00~` suffixed `01~`.
- [#13183](https://github.com/anthropics/claude-code/issues/13183) — closing `[201~` appears raw, repeated pastes hang.
- [#47745](https://github.com/anthropics/claude-code/issues/47745) — breaks OAuth code field at login.

**Practical rules:** `paste` for multi-line (>80 chars or contains `\n`); `write-chars` + `send-keys "Enter"` for short prompts; don't rely on bracketed paste for time-critical paths until upstream stabilizes.

### Control keys

- `send-keys "Ctrl c"` — interrupts current turn. Does NOT quit.
- Two `Ctrl c` in quick succession — exits the session.
- `send-keys "Ctrl d"` — exits at empty prompt.
- `send-keys "Esc"` — clears input / cancels modal.

### `claude -p` (print mode) for agent-to-agent

For one-shot "ask one question, get one answer" between agents: pipe stdin in, read stdout out, process exits. No PTY, no Ink, no submit-key problem. [Headless docs](https://code.claude.com/docs/en/headless) (stdin capped at 10 MB).

Use the interactive TUI when the conversation is long-lived and human-readable progress matters, or when you want `--resume` to work. Use `-p` when the caller is itself an agent.

### `--resume` and `--continue`

`claude --continue` (`-c`) reopens the most recent session for the cwd. `claude --resume SESSION_ID` reopens a specific one. Sessions stored as JSONL under `~/.claude/projects/...`. Two patterns this enables:

1. **Pause-and-inject:** Agent A is mid-task. `Stop` hook fires. Script appends context to a file. A restarted with `--resume` plus that file in the prompt. Continuation looks seamless.
2. **Read-only inspection:** Another script reads the transcript JSONL directly to see what A did, without disturbing A.

Sessions created with `-p` don't show in the interactive picker but ARE resumable by ID.

---

## 4. Reading Pane State

### `dump-screen`

`zellij action dump-screen [--path PATH] [--pane-id ID] [--full] [--ansi]`:

- Without `--full`: visible viewport only.
- With `--full`: viewport + scrollback.
- Without `--ansi`: plain text, escape sequences stripped.
- With `--ansi`: full ANSI preserved.
- Without `--path`: stdout. Without `--pane-id`: focused pane.

For "wait until claude is done thinking," dump periodically and look for a settled state (no diff between dumps N seconds apart, or appearance of the `>` prompt). This is what [zjctl](https://github.com/mrshu/zjctl)'s `wait-idle` does.

### Live mirror alternative

Launch a pane with `script -f /tmp/pane.log bash`. Every byte the terminal renders mirrors to that file in real time (with control sequences). `tail -f` gives a streaming source instead of polled snapshots. Heavier than `dump-screen` but useful for byte-perfect transcripts.

tmux contrast: `tmux capture-pane -p -S -` for full scrollback, `tmux pipe-pane -o 'cat >> file'` for live mirror. Zellij has only the polled version natively.

---

## 5. WASM Plugin API — Worth It?

Plugins run as WASM modules inside the zellij server with full plugin API: filesystem watching, plugin-to-plugin messaging, timer callbacks, intercepting keys. See [plugin API commands](https://zellij.dev/documentation/plugin-api-commands.html).

What plugins enable that `zellij action` can't:
- Event subscriptions — react to "pane created," "command exited" without polling.
- Custom UI rendered into a pane — driven by the zellij server, not a separate shell.
- Bidirectional pipes — `zellij pipe` is how external scripts talk to plugins; foundation `zjctl` uses for reliable RPC.

Cost: Rust + `wasm32-unknown-unknown`, build/distribute `.wasm`, debug across the boundary.

**For claude-bus:** stay in shell + `zellij action` through the early milestones. Adopt [zjctl](https://github.com/mrshu/zjctl) (don't build your own) once you need reliable wait-idle and structured pane addressing. Writing your own plugin only pays off if you need event subscriptions, which Claude Code hooks mostly substitute for.

---

## 6. Hooks as the Event Substrate

Configured in `settings.json`:

```json
{
  "hooks": {
    "PostToolUse": [
      { "matcher": "Bash|Edit|Write",
        "hooks": [{ "type": "command",
                    "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/log_event.sh" }] }
    ],
    "Stop": [
      { "hooks": [{ "type": "command",
                    "command": "${CLAUDE_PROJECT_DIR}/.claude/hooks/log_event.sh" }] }
    ]
  }
}
```

Each hook receives JSON on stdin. Exit 0 = proceed; exit 2 = block (stderr fed back to claude as the reason); other = non-blocking error. Stdout can be richer JSON (see `permissionDecision`, `additionalContext`).

### Events that matter for claude-bus

- **`SessionStart`** — fires on `startup`, `resume`, `clear`, `compact`. Source in the JSON. Use to register the agent.
- **`UserPromptSubmit`** — fires before claude sees the prompt. Can rewrite/block. Use for "did another agent send me a message? Prepend that context."
- **`PreToolUse`** / **`PostToolUse`** — around every tool call. PreToolUse can deny.
- **`Stop`** — claude believes it's done. Can be told to continue (`{"decision":"block","reason":"..."}`). Heartbeat for "agent went idle."
- **`SubagentStart`** / **`SubagentStop`** — same for Task-tool subagents.
- **`Notification`** — claude wants user attention.
- **`SessionEnd`** — exit. Clean up registry entry.

Full table in [hooks reference](https://code.claude.com/docs/en/hooks) (25+ event types).

### Injecting agent identity

Hook scripts inherit the claude process's env. Set the identity at pane launch in KDL:

```kdl
pane name="worker-1" command="bash" {
    args "-lc" "CLAUDE_BUS_AGENT_ID=worker-1 exec claude"
}
```

Every hook fired by that claude sees `$CLAUDE_BUS_AGENT_ID=worker-1`. Combined with `$ZELLIJ_PANE_ID`, unambiguous addressing.

### Shared event log template

```bash
#!/usr/bin/env bash
# settings/hooks/log_event.sh
LOG=/tmp/claude-bus/events.jsonl
mkdir -p "$(dirname "$LOG")"
jq -c \
  --arg agent "${CLAUDE_BUS_AGENT_ID:-unknown}" \
  --arg pane "${ZELLIJ_PANE_ID:-}" \
  --arg ts "$(date -u +%FT%TZ)" \
  '. + {agent: $agent, pane_id: $pane, ts: $ts}' \
  >> "$LOG"
```

Always-useful fields: `ts`, `agent`, `pane_id`, `session_id` (from hook input), `cwd`, `hook_event_name`.

### Concurrency: PIPE_BUF rule

POSIX guarantees `write(2)` of `≤ PIPE_BUF` bytes to a file opened `O_APPEND` is atomic. On Linux, `PIPE_BUF = 4096`. Bash `>>` opens `O_APPEND` and does a single `write` per `echo`/`printf`, so single-line appends < 4 KB never interleave. For larger or multi-line emissions:

```bash
( flock 200; jq -c '...' >> "$LOG" ) 200>"$LOG.lock"
```

NFS does NOT honor `O_APPEND` atomicity. Local filesystems only.

---

## 7. jj for This Repo

jj auto-snapshots the working copy on every command. Great for source, bad for files that change constantly.

### Tracking decisions

- **Track:** scripts, layouts, hook configs, `settings/settings.json`, `settings/hooks/*.sh`, `CLAUDE.md`, `docs/`.
- **Gitignore or out-of-repo:** event log, claim files, runtime registries. Prefer out-of-repo: `~/.local/state/claude-bus/$SESSION/events.jsonl` keeps jj uninvolved by definition.
- **Decide consciously:** `.claude/settings.local.json` → gitignore. `.claude/settings.json` → track if it exists.

### Performance

For small shell repos, auto-snapshot is instant. If an event log ends up tracked accidentally, you'll feel it. Set `fsmonitor.backend = "watchman"` early. See [jj config docs](https://docs.jj-vcs.dev/latest/config/).

### Commit cadence

Many tiny commits, one logical change each, imperative summary. `jj split` to break a snapshot post-hoc rather than being tidy upfront.

---

## 8. One Idea to Steal from Each Comparable Project

- **[disler/claude-code-hooks-mastery](https://github.com/disler/claude-code-hooks-mastery)** — UV single-file Python hooks (`#!/usr/bin/env -S uv run --quiet --script` with inline deps). Self-contained, no project dep pollution.
- **[Dicklesworthstone/claude_code_agent_farm](https://github.com/Dicklesworthstone/claude_code_agent_farm)** — Four-file coordination dir (`active_work_registry.json`, `completed_work_log.json`, `planned_work_queue.json`, `agent_locks/{agent}_{ts}.lock`) with stale-lock TTL.
- **[claude-squad](https://github.com/smtg-ai/claude-squad)** — Worktree-per-agent. Claude Code now has [built-in worktree support](https://code.claude.com/docs/en/worktrees) — lean on it.
- **[mrshu/zjctl](https://github.com/mrshu/zjctl)** — Selector grammar (`id:terminal:3`, `title:worker-1`, `cmd:/claude.*/`) and `wait-idle` primitive (poll `dump-screen`, watch for N identical outputs).
- **[avivsinai/agent-message-queue](https://github.com/avivsinai/agent-message-queue)** — Maildir-style queues (`new/`, `cur/`, `tmp/`). `rename(2)` is atomic on a single filesystem.
- **[anthropics Agent Teams](https://code.claude.com/docs/en/agent-teams)** — Split `~/.claude/teams/{team}/inboxes/` (per-agent queues) from `~/.claude/tasks/{team}/` (shared world state).

---

## 9. Sharp Edges (Will Bite You)

1. **`write-chars "...\n"` does not press Enter.** Always pair with `send-keys "Enter"`. ([#15553](https://github.com/anthropics/claude-code/issues/15553))
2. **Pane IDs reset per session.** Detach/reattach is fine; killing the session is not. Use layout `name` as canonical handle.
3. **Bracketed paste can leak / hang claude.** `paste` works for happy-path multi-line but has bugs ([#3134](https://github.com/anthropics/claude-code/issues/3134), [#13183](https://github.com/anthropics/claude-code/issues/13183)).
4. **`Stop` hooks can fire spuriously on empty final turns.** Don't treat every `Stop` as "task complete."
5. **Hooks don't fire in `claude -p` for some events.** Reduced coverage in print mode.
6. **jj auto-snapshot + noisy log file = `jj status` chokes.** Keep `events.jsonl` outside the repo.
7. **`PIPE_BUF` is 4096, not 64K.** Large hook emissions need `flock`.
8. **`zellij action` from cron silently no-ops** if `ZELLIJ_SESSION_NAME` isn't set. Pass `--session` or set the env explicitly.

---

## Sources

- [Zellij CLI Actions](https://zellij.dev/documentation/cli-actions)
- [Zellij CLI Recipes & Scripting](https://zellij.dev/documentation/cli-recipes.html)
- [Zellij possible actions](https://zellij.dev/documentation/keybindings-possible-actions.html)
- [Zellij creating a layout](https://zellij.dev/documentation/creating-a-layout)
- [Zellij swap layouts](https://zellij.dev/documentation/swap-layouts)
- [Zellij plugin API commands](https://zellij.dev/documentation/plugin-api-commands.html)
- [Zellij integration env vars](https://zellij.dev/documentation/integration.html)
- [Claude Code hooks reference](https://code.claude.com/docs/en/hooks)
- [Claude Code headless mode](https://code.claude.com/docs/en/headless)
- [Claude Code session management](https://code.claude.com/docs/en/sessions)
- [Claude Code worktrees](https://code.claude.com/docs/en/worktrees)
- [Claude Code Agent Teams](https://code.claude.com/docs/en/agent-teams)
- [Issue #15553 — programmatic input submission](https://github.com/anthropics/claude-code/issues/15553)
- [Issue #3134 — bracketed paste corruption](https://github.com/anthropics/claude-code/issues/3134)
- [Issue #13183 — bracketed paste hangs](https://github.com/anthropics/claude-code/issues/13183)
- [mrshu/zjctl](https://github.com/mrshu/zjctl)
- [Dicklesworthstone/claude_code_agent_farm](https://github.com/Dicklesworthstone/claude_code_agent_farm)
- [disler/claude-code-hooks-mastery](https://github.com/disler/claude-code-hooks-mastery)
- [avivsinai/agent-message-queue](https://github.com/avivsinai/agent-message-queue)
- [pipe(7) — atomicity / PIPE_BUF](https://man7.org/linux/man-pages/man7/pipe.7.html)
- [jj working copy](https://docs.jj-vcs.dev/latest/working-copy/)
- [jj configuration](https://docs.jj-vcs.dev/latest/config/)
