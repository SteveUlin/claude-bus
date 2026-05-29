# Claude Code Config, Skills, Hooks & Ops — A Deep Reference for claude-bus

> Written 2026-05-28. The deep companion to `docs/modern-agent-techniques.md`,
> scoped to **one topic: how we configure the Claude Code harness itself** —
> CLAUDE.md, `settings.json`, hooks, skills, slash commands, roles, and the
> ops layer (scheduling, monitoring, telemetry). Every section leads with the
> **principle** — the mechanism, the tradeoff, the design pressure — then the
> concrete specifics and what the best implementations actually do. Skim the
> tables; read the "How this maps to claude-bus" section last.
>
> This builds on `docs/claude-md-conventions.md` (voice/length craft — not
> repeated here) and goes deeper on the *machinery*: the full hook protocol as
> it stands in 2026 (which has roughly **doubled** since that doc was written),
> hook design patterns, skills-vs-commands-vs-subagents, settings schema craft,
> and the scheduled/observed ops layer.

claude-bus is unusual among Claude Code harnesses: its config is **fleet-wide
and symlinked**, its agents are **durable panes** not ephemeral `-p` jobs, and
it already drives a **hand-built broker** rather than leaning on hook-only
comms. That shapes which ecosystem patterns are worth stealing. The community
has converged on three reusable config primitives — *deterministic hooks*,
*progressive-disclosure skills*, and *headless scheduled agents* — and
claude-bus uses the first heavily, the second lightly, and the third not at
all. The biggest gaps are (1) we are leaving a large, **newly-expanded hook
surface** unused, (2) our hooks emit but never *decide* (no `additionalContext`
injection, no blocking), and (3) we have no ops layer (telemetry, scheduled
reflection, health cron) despite owning the perfect substrate for it.

---

## 1. Core principles & tradeoffs

### 1.1 The config layering model — what loads, when, and what it costs

**The principle: every config surface is a different point on the
{determinism × context-cost × scope} space.** Pick the surface by *who must
act and when*, not by convenience.

| Surface | Determinism | Context cost | When it acts | Scope knob |
|---|---|---|---|---|
| **`settings.json` hooks** | **Total** (shell runs regardless of model) | ~0 tokens (runs out-of-band) | Fixed lifecycle points | precedence chain (below) |
| **CLAUDE.md** | Probabilistic (model may drop it) | **Every turn, forever** | Whenever model "remembers" | dir nesting + `@`-includes |
| **`.claude/rules/*.md`** | Probabilistic, but path-scoped | Only when path matches | On matching file touch | `paths:` frontmatter |
| **Skills** | Probabilistic invoke, deterministic body | **Only when invoked** (progressive disclosure) | On trigger match | per-skill `allowed-tools` |
| **Slash commands** | User-triggered, deterministic expansion | Only when typed | On `/name` | args |
| **Subagents** (`.claude/agents`) | Isolated context | Separate window | On Task dispatch | own tools/model |

The load-bearing insight, straight from Anthropic's memory doc and reinforced
in our own `claude-md-conventions.md`: **if an instruction must fire every time,
it does not belong in CLAUDE.md — it belongs in a hook.** CLAUDE.md is
probabilistic and bills context every turn for the life of the file; a hook is
deterministic and bills nothing. This is the single most under-exploited lever
in claude-bus's config (see §6).

**Settings precedence** (highest wins): enterprise/managed policy → command-line
flags → `.claude/settings.local.json` (project-local, gitignored) → project
`.claude/settings.json` → user `~/.claude/settings.json`
([settings docs](https://code.claude.com/docs/en/settings)). claude-bus
deliberately collapses this: one canonical `settings/claude-settings.json`
symlinked into every workspace's `.claude/settings.json` (gitignored), so a
single edit applies fleet-wide. This is the 2026 best practice ("treat your
config like a dotfiles repo: small, opinionated, version-controlled") applied
to a *fleet* instead of a person.

### 1.2 The CLAUDE.md length/compliance tradeoff (ours is long)

**The principle: instruction-following degrades with instruction count, not
just token count.** A model reliably tracks a bounded set of distinct
directives; past that, *adding* a rule *lowers* compliance with the others
because the important ones get diluted. Anthropic's hard guidance is **under
200 lines per CLAUDE.md**; the community ceiling for *high-signal* content is
~80–120 lines. Our `docs/claude-md-conventions.md` already nails the voice
craft (imperative, verifiable, affirmative, scannable) — the gap is *volume*.

The repo `CLAUDE.md` is ~190 lines but a large fraction is **discoverable
documentation, not instruction**: it reproduces the entire `bus help` output
(the "Commands" section), the full broker spec (topic kinds, cursor semantics,
reliability), and status-snapshot prose ("Implementation began 2026-05-24…").
`claude-md-conventions.md` already scores these as Violations 1–5 with
proposed rewrites. The principle behind every one of those cuts: **"Would
removing this line cause Claude to make a mistake?" If not, cut it** — and
*"anything Claude can figure out by reading code or running `bus help`"* is by
definition droppable. The broker spec is reference material; it belongs in
`docs/` reached by an `@`-include, not in the always-loaded file.

**The tiering discipline** (the actionable shape):
- **CLAUDE.md** = inviolable rules that must hold *every turn*: jj-not-git,
  edit `settings/` not `.claude/`, no-secrets, the four code-change principles.
- **`docs/*.md`** = deep reference, `@`-linked and loaded on demand.
- **`.claude/rules/*.md`** with `paths:` frontmatter = rules that fire only on
  matching files (e.g. a `src/**.cpp` rule carrying the C++ style).
- **Hooks** = anything that must run regardless of what the model decides.

> **Skeptic's note.** Don't over-shrink. The four "Code changes" principles and
> the jj convention earn their lines — they shape behavior on *every* edit.
> The target is removing *documentation*, not *direction*.

### 1.3 Determinism is the whole point of hooks

**The principle: a hook is code at a lifecycle point — it does not depend on
the model's interpretation, so it cannot be "forgotten," prompt-injected away,
or skipped under context pressure.** That is exactly why "run X every time"
belongs in a hook. The control surface is richer than a binary: a hook
communicates back to Claude through **exit codes** *and* **structured JSON on
stdout**.

The exit-code contract (universal across events):
- **Exit 0** → success; stdout parsed as JSON if present.
- **Exit 2** → *blocking* error; stderr is fed back to Claude as feedback (the
  semantics differ per event — see §2).
- **Other** → non-blocking error; stderr logged, execution continues.

The JSON contract (the real power): a hook can emit `additionalContext` (wrapped
in a system-reminder and injected into the conversation), `decision: "block"`,
`permissionDecision: allow|deny|ask|defer`, `systemMessage`, `continue: false`,
and event-specific fields. **claude-bus uses none of this** — every one of our
hooks exits 0 with no JSON, used purely as a fire-and-forget logging tap. That
is a deliberate, defensible choice (the broker, not the hook, decides delivery)
but it leaves the *decision* surface entirely on the table.

---

## 2. The hook protocol in 2026 — the full surface (and how much it grew)

**The principle: the hook surface is the harness's public API for
deterministic control, and it expanded dramatically in the last year.** When
`docs/modern-agent-techniques.md` was written it listed ~7 events. The current
[hooks reference](https://code.claude.com/docs/en/hooks) defines **~30**. Many
of the new ones are directly relevant to a multi-agent harness. The table below
is the dense version — events claude-bus could exploit are **bold**.

| Event | Fires | Key input fields | Key output / control | Why claude-bus cares |
|---|---|---|---|---|
| **SessionStart** | session begin/resume | `source` (startup\|resume\|clear\|compact), `model`, `agent_type`, `session_title` | `additionalContext`, **`initialUserMessage`**, `sessionTitle`, **`watchPaths`**, **`reloadSkills`** | inject `learnings.md`; seed first turn; set pane title |
| **SessionEnd** | session terminates | `end_reason` (clear\|resume\|logout\|prompt_input_exit\|bypass_permissions_disabled\|other) | none (informational) | deregister agent (we do this) |
| Setup | `--init`/`--maintenance` in `-p` | `trigger` | `additionalContext`, `continue` | CI/maintenance scripts |
| InstructionsLoaded | CLAUDE.md/rules load | `file_path`, `memory_type`, `load_reason` | none | *measure* what context actually loaded |
| **UserPromptSubmit** | user submits prompt | `prompt`, `permission_mode` | `decision:"block"`, **`additionalContext`**, `suppressOriginalPrompt`, `sessionTitle` | inject mail; ack-detection; **30s timeout** |
| UserPromptExpansion | slash cmd expands | `command_name`, `command_args` | block, `additionalContext` | gate/audit slash commands |
| **PreToolUse** | before a tool call | `tool_name`, `tool_input`, `permission_mode` | **`permissionDecision`** (allow\|deny\|ask\|defer), `updatedInput`, `additionalContext` | security gating, input rewrite |
| PermissionRequest | permission dialog | `tool_name`, `tool_input` | `decision.behavior` allow/deny, `updatedInput` | auto-approve safe ops |
| PermissionDenied | auto-mode denial | `tool_name` | `retry: bool` | recover from denials |
| **PostToolUse** | tool succeeds | `tool_name`, `tool_input`, `tool_result` | `decision:"block"`, `additionalContext` | our focus-write; lint/format |
| **PostToolUseFailure** | tool fails | `tool_name`, `error` | block, `additionalContext` | feed errors back to fix |
| PostToolBatch | after a parallel batch | `tool_calls[]` | block | batch-level checks |
| **Stop** | Claude finishes a turn | **`tool_calls[]`** (new) | `decision:"block"` (= don't idle), `additionalContext` | our drain/ack; keep-working |
| **StopFailure** | turn ends on API error | `error_type`, `error_message` | none | **surface rate-limit/billing to ops** |
| **SubagentStart** | subagent spawned | `agent_type`, `agent_id`, `prompt` | `additionalContext` | track in-context fan-out |
| **SubagentStop** | subagent finishes | `agent_type`, `agent_id` | block (= don't stop) | TTS/notify on done |
| **TaskCreated** | TaskCreate runs | `task_id`, `task_name` | block (= rollback) | richer than parsing tool_input |
| **TaskCompleted** | task → completed | `task_id`, `task_name` | block | drive focus-write off this |
| **TeammateIdle** | agent-team teammate idles | `teammate_name` | block (= prevent idle) | native multi-agent hook! |
| ConfigChange | config file changes mid-session | `config_source`, `file_path` | block (except policy) | detect settings drift |
| CwdChanged | working dir changes | `previous_cwd`, `new_cwd` | — (has `CLAUDE_ENV_FILE`) | guard workspace boundary |
| FileChanged | watched file changes on disk | `file_path`, `change_type` | `additionalContext` | react to external writes |
| WorktreeCreate/Remove | `--worktree`/isolation | `base_path`, `worktree_path` | stdout = path | jj-workspace integration |
| **PreCompact** | before compaction | `trigger` (manual\|auto) | block (= prevent), `additionalContext` | preserve invariants |
| **PostCompact** | after compaction | — | `systemMessage` | re-inject what compaction dropped |
| Notification | CC emits a notification | `notification_type`, `message` | `systemMessage` | desktop/TTS alerts |
| MessageDisplay | assistant text displayed | `message_text` | `displayContent` (replaces *display only*) | redact/annotate the pane view |
| Elicitation / ElicitationResult | MCP requests input | `server_name`, `form_schema` | accept\|decline\|cancel + `content` | auto-fill MCP prompts |

**Three field-level details that matter for us:**

1. **`Stop` now carries `tool_calls[]`.** Our `agent_status` state detection
   and the broker's blocking-slash ACK can read the *structured* last-turn tool
   list directly instead of inferring from pane dumps. This is the typed-signal
   that `modern-agent-techniques.md §1.2` argued for, now available natively.

2. **`UserPromptSubmit` has a 30s timeout** (vs 600s for most events). Our
   `log-event.sh` runs `jq` + `date` synchronously on this hot path; it's fast
   today, but any future `UserPromptSubmit` drain logic must stay well under
   30s or it wedges *every* human turn. Hooks on this event are the riskiest.

3. **`SessionStart` gained `watchPaths` and `reloadSkills`.** A SessionStart
   hook can now register file watches *and* force a skill reload — the delivery
   mechanism for a "skills that grow" library (§4) that updates without a
   session restart.

**The new multi-agent-native events** — `TeammateIdle`, `SubagentStart/Stop`,
`TaskCreated/Completed` — are Anthropic building the *exact* coordination
signals claude-bus reconstructs by hand. `TeammateIdle` in particular is a
native "agent is about to go idle, here's a chance to feed it work" hook; that
is precisely the moment our broker wants to push queued mail. We should track
whether Claude Code's own "agent teams" primitive converges with our bus.

---

## 3. How the best implementations actually do hooks

### 3.1 The security-gating PreToolUse pattern (the canonical example)

**The principle: `PreToolUse` + exit 2 is a deterministic veto on tool calls —
the one place you can stop the model before it acts.** The reference
implementation is `disler/claude-code-hooks-mastery`, the most-copied hook
collection. Its `PreToolUse` hook regex-scans `tool_input` and blocks
destructive shell and secret access:

> blocks `rm\s+.*-[rf]`, `sudo\s+rm`, `chmod\s+777`, `>\s*/etc/`, and any
> access to `.env` files — "Exit code 2 blocks the entire tool call and feeds
> error messages to Claude" ([claude-code-hooks-mastery](https://github.com/disler/claude-code-hooks-mastery)).

The 2026 evolution moves past regex to **AST-based parsing**: `Dippy`
(awesome-claude-code) "auto-approves safe bash commands using AST-based parsing
while prompting for destructive operations — reduces permission fatigue without
disabling safety." And `parry` (Dmytro Onypko) "scans tool inputs and outputs
for injection attacks, secrets, and data exfiltration." `TDD Guard` (Nizar
Selander) blocks edits that violate test-first discipline. The shared shape:
**`PreToolUse` reads `tool_input`, decides via `permissionDecision`, and
explains via `permissionDecisionReason`.**

### 3.2 The Stop-hook "keep working" / self-pull pattern

**The principle: `Stop` + exit 2 means "don't go idle — here's more to do."**
This is the mechanism behind every "autonomous loop" harness. Our own
`settings/hooks/examples/loop-mailbox.sh` is a textbook instance: it drains the
agent's mailbox on `Stop`, and *if anything was queued*, prints it to stderr
and exits 2, so the agent treats the mail as a new turn and keeps working until
the box empties. `disler`'s Stop hook does the cosmetic version (LLM-generated
completion message + TTS). The "Ralph Wiggum" family (`ralph-orchestrator`,
`ralph-wiggum-bdd`) generalizes it: loop a `claude -p` against a prompt file
until a completion marker appears, with **circuit-breaker / rate-limit
guardrails against infinite loops** — the critical safety addition the naive
version lacks.

**The tradeoff:** a Stop-exit-2 loop with no exit condition is an infinite
token burn. Every production implementation pairs it with a **bounded
condition** (mailbox empty, marker found, max-iterations, rate-limit budget).
claude-bus's broker is the bound — it only re-injects when a record exists —
which is why the self-pull pattern is safe in our `loop-mailbox.sh`.

### 3.3 Hook-only multi-agent comms (HCOM) — the architecture we *didn't* pick

**The principle: you can build an entire multi-agent bus out of hooks alone,
with no daemon — at the cost of delivery latency and control.** `HCOM`
("Claude Code Hook Comms", aannoo) "enables real-time multi-agent collaboration
using hooks with @-mention targeting and live dashboard monitoring." The shape
(reconstructed from its description and the genre): a shared message file;
agents write `@name`-targeted messages; a `Stop`/`UserPromptSubmit` hook reads
the file, filters for messages addressed to the running agent, and re-injects
them as `additionalContext`. No TTY write, no broker — the *agent's own hook*
pulls its mail.

**This is the "FIFO-drained-by-agent" transport that
`modern-agent-techniques.md §1.1` recommended as the strictly-better channel,
already shipping in the wild.** claude-bus chose a broker instead because the
broker buys *centralized* control: retry/ack/escalation, in-flight tracking,
typed topics, human-attachable panes, and a single audit log. HCOM's
hook-only design buys *zero-daemon simplicity* and a race-free channel but
gives up central scheduling and the in-flight/DLQ machinery. The honest
read: **HCOM validates the hybrid — we should adopt its agent-side pull as a
*delivery path*, keep the broker as the *scheduler*.** (This is exactly the
hybrid in `modern-agent-techniques.md`, with HCOM as existence proof.)

### 3.4 Notification & statusline craft

**The principle: out-of-band signals (sound, desktop, statusline) carry state
the human needs without stealing the pane.** This is the *same* design pressure
as claude-bus's "second observability channel."

- **Notification hook → desktop/TTS.** `CC Notify` (dazuiba): desktop
  notification on input-needed/task-done with one-click return to the editor.
  `Claudio` (Christopher Toth): OS-native sounds. `disler`'s Notification hook:
  TTS "Your agent needs your input," 30%-chance personalized. The mechanism is
  trivial — the Notification event carries `notification_type` ∈
  {`permission_prompt`, `idle_prompt`, …} and `message`; the hook shells out to
  `notify-send`/`paplay`/`say`.
- **Statusline = the persistent per-pane HUD.** The community has gone deep
  here: `CCometixLine` (Rust, git + usage), `ccstatusline`, `Claude HUD`
  (context %, tools, agents, todos), `claude-pace` (rate-limit burn delta, 5h/7d
  usage %, context-window %), `claudia-statusline` (SQLite-persisted stats).
  The statusline command receives the same session JSON that carries **live
  token counts** (the only place hooks *can't* see them — per our memory note
  "Claude Code token data sources"). claude-bus already wires a
  `statusLine` command and renders per-agent bars via `bus agent-bar`; the
  ecosystem shows how much richer a single line can be (burn rate, context %,
  cost).

### 3.5 The git `prepare-commit-msg` trick (we already do this)

Our `settings/git-hooks/prepare-commit-msg` appends `Co-Authored-By:
<agent-id>` when `$CLAUDE_BUS_AGENT_ID` is set — making per-agent authorship
visible in `jj log`/blame and diagnosing the shared-tree scoop-bundle pattern.
This is a *git* hook, not a Claude Code hook, and it's a clean example of the
broader principle: **push identity/provenance into the artifact at the
deterministic boundary** rather than asking the model to remember to sign.

---

## 4. Skills, slash commands & subagents — the right tool

**The principle (the distinction that actually matters):**

| Primitive | What it is | Context behavior | Use when |
|---|---|---|---|
| **Slash command** | a prompt template (`.claude/commands/*.md`) | inserts text into the current context | a reusable *prompt* — cheap, no isolation |
| **Skill** | named instruction bundle with **progressive disclosure** | loads body only when triggered; can declare `allowed-tools` | a reusable *procedure* with its own tools |
| **Subagent** | `.claude/agents/*.md` via the Task tool | **separate context window** | context isolation / parallelism |
| **Headless `claude -p`** | a whole fresh process | own session | background/scheduled work, no human surface |

The decision rule that fits claude-bus's "pane = mailbox" memory:
**bus = durable/attachable peers; subagent = ephemeral in-context isolation;
`claude -p`/dynamic-workflow = bounded mass fan-out with no human watching.**

**Our `dispatch`/`draft`/`peek`/`status` are slash commands stored in
`.claude/commands/` but described in the harness as skills** — they're prompt
templates with frontmatter (`description`, `argument-hint`). That's correct for
what they do: orchestration prompts that expand into the comms agent's context.
The frontmatter `argument-hint`/`description` is the documented slash-command
schema. They are well-written: each ends with an explicit approval gate and a
"never auto-send" guard, which is exactly the deterministic-floor-in-a-
probabilistic-tool pattern.

**The skill-specific lever we don't use: `allowed-tools` in frontmatter and
progressive disclosure.** A skill can preload only the tools it needs and load
its body only on trigger — so a heavy procedure costs zero context until used.
Our roles (`roles/*.md`) carry `tools:` and `model:` frontmatter, but
`agent-launch` **strips the YAML frontmatter** before passing the body via
`--append-system-prompt-file` — so for these top-level pane agents the `tools:`
and `model:` lines are **decorative, not enforced** (see §6 flaw). That
frontmatter only binds when a file is loaded *as* a subagent/skill, not as an
appended system prompt.

**The "meta-agent" and "skills that grow" patterns.** `disler`'s meta-agent
(`.claude/agents/meta-agent.md`) generates new subagents from a description.
The self-improving-skills pattern (MindStudio) maintains an external
`learnings.md` injected at runtime and updated after each run — *"five to ten
run cycles before clear behavioral changes."* For claude-bus this is the
durable-agent superpower: a reflection pass writes per-agent `learnings.md`,
`SessionStart` injects it via `additionalContext`, and `reloadSkills` (new
SessionStart output) lets new skills land without a restart. The 2026 consensus
on hygiene: **8–12 skills, prune anything not triggered in 30 days.**

---

## 5. Settings schema craft & the ops layer

### 5.1 settings.json craft

**The principle: settings.json is the only fleet-wide deterministic surface, so
put everything that must be uniform there — and nothing secret.** Beyond hooks,
the high-value keys:

- **`permissions.allow` / `deny`** with rule syntax — `Bash(git diff *)` (note
  the space before `*` for prefix matching; `Bash(git diff*)` would also match
  `git diff-index`). The `/fewer-permission-prompts` skill scans transcripts and
  auto-builds an allowlist. claude-bus runs `--dangerously-skip-permissions`
  fleet-wide (in `agent-launch`), so we sidestep this entirely — a deliberate
  trust-boundary call documented in our memory, but it means the
  `permissions`/`PreToolUse` *safety* surface is unused.
- **`env`** — uniform env vars for every agent. **This is where OpenTelemetry
  exporter vars belong** (§5.3).
- **`statusLine`** — we set it (to `~/.claude/statusline-command.sh`).
- **`model`**, **`theme`**, **`outputStyle`**, **`cleanupPeriodDays`** — uniform
  defaults.
- **`--bare` mode** (new): `claude --bare -p` skips auto-discovery of hooks,
  skills, plugins, MCP, memory, and CLAUDE.md — *"the recommended mode for
  scripted and SDK calls."* Critical for **scheduled/cron agents**: a reflection
  cron should run `--bare` so it's reproducible and doesn't drag the whole
  fleet config into a background job.

### 5.2 Scheduled agents: `/schedule` vs `/loop` vs cron

**The principle: pick the scheduler by *where it must run and whether it
survives your machine being off*.** Three distinct tools, constantly confused
([wmedia](https://wmedia.es/en/tips/claude-code-schedule-vs-loop-vs-cron)):

| Tool | Runs | Persistence | Interval | Sees | Use for |
|---|---|---|---|---|---|
| **`/loop`** | local, in active session | 7-day auto-expiry, needs session open | any | live working tree | monitoring *while you work* |
| **`/schedule`** (Routines) | **Anthropic cloud**, machine off | indefinite | **1-hour min** | fresh repo clone, no uncommitted | daily tasks, offline |
| **cron + `claude -p`** | local OS scheduler | indefinite | any | whatever you give it; "starts from scratch" | edge cases the other two miss |

For claude-bus the relevant pair is **cron + `claude --bare -p`** (local,
because the broker and panes are local) and **`/loop`** (which our `comms` role
already uses: *"At SessionStart, invoke `/loop 30s bus msg fetch inbox-comms`"*
as a delivery fallback). The reflection/health/DLQ-summary jobs from
`modern-agent-techniques.md §3.6` are textbook **local cron + `claude --bare
-p`** jobs — headless, no human surface, reproducible. Example shape:

```bash
# nightly reflection per agent (cron)
0 3 * * *  cd /home/sulin/claude-bus && \
  claude --bare -p "Read today's events.jsonl for agent elodin; distill what \
  wedged and what worked into roles/learnings/elodin.md" \
  --allowedTools "Read,Edit,Bash(jq *)" > /tmp/claude-bus/reflect.log 2>&1
```

> **Note for June 2026:** Agent SDK and `claude -p` usage on subscription plans
> draws from a *separate monthly Agent SDK credit* starting 2026-06-15. Cron
> reflection jobs will bill against that pool, not interactive limits — budget
> accordingly.

### 5.3 Monitoring people actually run

**The principle: the monitoring that survives is the monitoring that
*composes* — a standard beats a bespoke dashboard.** The ecosystem splits into
three layers, and claude-bus has built the *bespoke* layer (`bus monitor`,
`agent-bar`, `deck`) but skipped the *standard* layer:

1. **Statusline HUDs** (per-pane, live) — `Claude HUD`, `claude-pace`,
   `claudia-statusline` (SQLite). claude-bus = `agent-bar` (our equivalent).
2. **Usage/cost analyzers** (post-hoc, from local logs) — `CCUsage`, `ccflare`
   (web dashboard), `ccxray` (transparent HTTP proxy + context-window viz),
   `Claude Code Usage Monitor` (live burn-rate + depletion prediction). These
   read the same JSONL transcripts our watcher tails.
3. **OpenTelemetry GenAI export** (the standard) — Claude Code has **built-in
   OTel instrumentation**: spans around each model request and tool execution,
   token/cost counters, OTLP export to Honeycomb/Datadog/Grafana/Langfuse. It's
   `env`-var-activated — exactly what `settings.json`'s `env` block is for. This
   is `modern-agent-techniques.md §3.5`; the config-layer takeaway is that it's
   **a settings.json `env` change, not new code.**

`claude-devtools` (matt1398) is the notable observability tool for *our* shape:
"turn-based context data, compaction visualization, **subagent execution
trees**, custom notification triggers" — the cross-agent view from a desktop
app reading the logs.

---

## 6. How this maps to claude-bus

The unifying read: **we use hooks as a pure logging tap and CLAUDE.md as a
reference manual — both leave their highest-leverage surfaces unused.** Hooks
can *decide* (we never emit JSON); CLAUDE.md should *only* direct (we pad it
with docs). And we own the perfect durable substrate for an ops layer (reflect,
telemetry, health) but run none of it.

### Flaws I spotted in current config (with file refs)

- **`roles/*.md` frontmatter is decoratively dead.** `roles/comms.md` declares
  `tools: Bash, Read, Grep, AskUserQuestion` and `model:
  claude-3-5-sonnet-20241022`; `roles/bast.md` declares `model:
  claude-sonnet-4-6`. But `bin/agent-launch` (lines ~204–209) **strips YAML
  frontmatter** and passes only the body via `--append-system-prompt-file`. The
  pane's model comes from claude's default/CLI, *not* the role file — so
  `comms` is **not** pinned to Sonnet 3.5, and `tools:` does **not** restrict
  the comms agent (which the role prose claims: *"Your `tools:` deliberately
  omit Edit and Write"* — false at the harness level; only the prose deters).
  Either enforce it (pass `--model` from the parsed frontmatter in
  `agent-launch`, and rely on the prose ban for tools since top-level agents
  can't be tool-gated like subagents) or delete the misleading frontmatter.
- **`comms.md` claims a tool restriction the harness doesn't enforce.**
  `roles/comms.md:176` ("No file edits. Your `tools:` deliberately omit Edit
  and Write") describes a guarantee that isn't real for an appended-system-
  prompt agent. The behavioral ban works (prose is persuasive) but the *stated
  mechanism* is wrong. Fix the wording to "you are instructed not to edit" or
  make it real via a `PreToolUse` deny hook scoped to the comms agent.
- **`loop-mailbox.sh` references a legacy binary.** It calls
  `$BUS_ROOT/bin/mailbox` (line 14/20), but the architecture moved to the
  unified `bin/bus` broker; `bin/mailbox` is listed only as a gitignored
  compiled artifact. This example hook is stale relative to the broker design
  and would mislead anyone copying it. Update to `bus msg fetch` or mark it
  clearly as historical.
- **`UserPromptSubmit` hot-path cost.** `log-event.sh` runs `jq` + a subshell
  `date` on `UserPromptSubmit`, which has a **30s timeout** (not 600s). Fine
  now, but the doc/comment should flag that this event is the latency-critical
  one — any future drain/ack logic added here must stay fast or it wedges every
  human turn.
- **CLAUDE.md carries `bus help` + the full broker spec.** Already scored as
  Violations 1, 2, 5 in `docs/claude-md-conventions.md`. The principle: move
  the broker spec to an `@`-linked `docs/` file; the always-loaded CLAUDE.md
  should hold only every-turn rules. Cost is real: the spec bills context on
  *every turn of every agent forever*.

### Recommendations (principle-first, with effort/payoff)

1. **SessionStart → inject per-agent `learnings.md` via `additionalContext`**
   (principle: durable agents can learn over time; the injection point is a
   documented hook output). Pair with a nightly cron reflection job
   (`claude --bare -p`) that distills `events.jsonl` into the file. **Effort:
   low–med. Payoff: high** — the superpower headless fan-out can't have.
2. **OpenTelemetry export via `settings.json` `env` block** (principle: a
   standard composes where bespoke dashboards don't). Flip the OTel env vars in
   the canonical settings file; every agent exports uniformly. **Effort: low.
   Payoff: high** — cross-agent cost/latency/token-anomaly, queryable.
3. **Adopt the hook-only *pull* path (HCOM-style) as a delivery fallback**
   (principle: the agent draining its own mail via `UserPromptSubmit`/`Stop`
   `additionalContext` is race-free and TTY-free). Keep the broker as
   scheduler; let the hook be the transport. Our `loop-mailbox.sh` is half of
   this already. **Effort: med. Payoff: very high** — kills the TTY race the
   whole prior doc centers on.
4. **Tier CLAUDE.md: move the broker spec + `bus help` to `@`-linked `docs/`**
   (principle: every-turn context must earn its slot; reference doesn't).
   **Effort: low. Payoff: med** — better adherence, lower per-turn cost.
5. **Fix the dead role frontmatter** — either enforce `--model` in
   `agent-launch` or strip the misleading `tools:`/`model:` lines and correct
   the comms prose (principle: config should not claim guarantees the harness
   doesn't keep). **Effort: low. Payoff: med** — removes a correctness landmine.
6. **A `PreToolUse` safety hook *scoped to coordinator agents*** (comms, auri):
   deny `Edit`/`Write` deterministically so the "no file edits" rule is real,
   not just persuasive (principle: must-always-fire → hook, not prose). **Effort:
   low. Payoff: med** — and it's the one place our `--dangerously-skip-
   permissions` posture still wants a guardrail.
7. **A Notification hook → desktop/sound on `idle_prompt`/`permission_prompt`**
   (principle: out-of-band signal without stealing the pane — same as our
   second-channel philosophy). **Effort: low. Payoff: med** — "is an agent
   waiting on me?" answered without tab-hunting.
8. **Surface `StopFailure` to ops** (principle: API-level failures —
   rate-limit, billing, server-error — are invisible to `Stop` but carried by
   `StopFailure`). Route its `error_type` into `inbox-ops`/`audit`. **Effort:
   low. Payoff: med** — turns silent rate-limit stalls into a visible event.
9. **Drive `focus-write` off `TaskCreated`/`TaskCompleted` events** instead of
   regex over `PostToolUse` tool_input (principle: typed lifecycle events beat
   parsing tool payloads — same as the reducer argument). The new events carry
   `task_id`/`task_name` directly. **Effort: low. Payoff: low–med** — simpler,
   drift-proof `focus-write.sh`.
10. **Watch `TeammateIdle` / agent-team primitives** (principle: Anthropic is
    shipping native multi-agent coordination signals that overlap our broker).
    Not an action yet — a tracking item. If `TeammateIdle` matures, it's the
    native "push queued mail now" trigger we hand-build. **Effort: n/a.
    Payoff: strategic.**

### What to deliberately NOT do

- **Don't move delivery wholesale into hooks (full HCOM).** We'd lose the
  broker's retry/ack/DLQ/in-flight machinery and central audit. Adopt the *pull
  path* as a fallback, keep the broker as scheduler.
- **Don't grow CLAUDE.md.** Every line bills every turn of every agent forever.
- **Don't add slow logic to `UserPromptSubmit` hooks** (30s timeout, every-turn
  latency).
- **Don't rely on role-file `tools:`/`model:` frontmatter** for top-level pane
  agents — it's stripped. Enforce in `agent-launch` or drop it.

---

## Sources

**Anthropic primary**
- [Hooks reference (full ~30-event protocol)](https://code.claude.com/docs/en/hooks)
- [Run Claude Code programmatically / headless (`-p`, `--bare`, stream-json)](https://code.claude.com/docs/en/headless)
- [Settings & permission rule syntax](https://code.claude.com/docs/en/settings)
- [Memory / CLAUDE.md](https://code.claude.com/docs/en/memory)
- [Best practices](https://code.claude.com/docs/en/best-practices)
- [Skills](https://code.claude.com/docs/en/skills)
- [OpenTelemetry observability](https://code.claude.com/docs/en/agent-sdk/observability)

**Hook / skill / command collections**
- [awesome-claude-code (resources table)](https://github.com/hesreallyhim/awesome-claude-code) — Britfix, Dippy (AST permission), parry (injection scan), TDD Guard, HCOM, Claudio, CC Notify
- [disler/claude-code-hooks-mastery](https://github.com/disler/claude-code-hooks-mastery) — canonical PreToolUse security gating, Stop/TTS, meta-agent, output-styles
- [HCOM — Claude Code Hook Comms (aannoo)](https://github.com/aannoo) — hook-only multi-agent bus, @-mention targeting

**Statusline / monitoring**
- Claude HUD, claude-pace, claudia-statusline, CCometixLine, ccstatusline (awesome-claude-code Status Lines)
- CCUsage, ccflare, ccxray, Claude Code Usage Monitor, claude-devtools (awesome-claude-code Usage Monitors)

**Scheduling**
- [/schedule vs /loop vs cron — wmedia](https://wmedia.es/en/tips/claude-code-schedule-vs-loop-vs-cron)
- [Claude Code Scheduled Tasks guide — claudefa.st](https://claudefa.st/blog/guide/development/scheduled-tasks)
- [Headless mode / autonomous agents — MindStudio](https://www.mindstudio.ai/blog/claude-code-headless-mode-autonomous-agents)

**Self-improving skills / memory**
- [Self-improving Claude Code skills — MindStudio](https://www.mindstudio.ai/blog/self-improving-ai-skills-claude-code)
- [Best Claude Code skills 2026 — Developers Digest](https://www.developersdigest.tech/blog/best-claude-code-skills-2026)

**In-repo companions**
- `docs/claude-md-conventions.md` (voice/length craft — not repeated here)
- `docs/modern-agent-techniques.md` (the broad survey this deepens)
