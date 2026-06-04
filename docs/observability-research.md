# Observability research — per-agent tracking + orchestrator docs

> **FROZEN — pre-refactor research (Phase-4 doc cleanup, 2026-06-03).**
> Broad prior-art survey, cited by-section across the frozen `deep/`
> references. Retained with that corpus rather than folded into
> `docs/prior-art.md` (folding would orphan those citations); re-evaluated
> after the broker-seam refactor (Phase 2) lands. Historical context, not
> current truth.

Author: elodin · 2026-05-27 · For: comms / sulin
Status: research only; no implementation. Pick a design after reading.

Two threads commissioned by comms:
1. How should the fleet expose per-agent **tokens / focus / cwd**?
2. Does **comms** need its own CLAUDE.md-style file?

---

## Thread 1 — fleet observability

### 1.1 What Claude Code actually exposes

I checked three surfaces against the running fleet's data and the
authoritative docs.

**Hooks payload — no native token data.** Walking
`/tmp/claude-bus/events.jsonl` (890 lines, fleet-wide), no record at
any event (`PreToolUse`, `PostToolUse`, `Stop`, `UserPromptSubmit`,
`SessionStart`) carries `input_tokens` / `output_tokens` /
`cache_*_tokens`. The hooks reference confirms this — the documented
PostToolUse / Stop fields cover `tool_input`, `tool_response`,
`stop_hook_active`, `last_assistant_message`, but no usage block.
What hooks do give us, on every event:
- `payload.cwd` — current working dir (already what we want)
- `payload.transcript_path` — pointer to the per-session JSONL
- `payload.session_id` — stable session UUID
- `payload.last_assistant_message` (Stop only) — last reply text
- `payload.prompt` (UserPromptSubmit only) — the prompt text
- `payload.tool_name` + `payload.tool_input` (Pre/PostToolUse)

**Transcript JSONL — full usage per turn.** Each assistant line in
`~/.claude/projects/-home-sulin-claude-bus/<uuid>.jsonl` carries
`message.usage.{input_tokens, output_tokens, cache_creation_input_tokens,
cache_read_input_tokens}` and `message.model`. Sum across lines for
cumulative; tail one line for the most recent turn. This is the data
source every community usage tool reads (see 1.3).

**Statusline JSON — pre-computed live snapshot.** Discovery: Claude
Code already pipes a rich JSON object into any configured statusline
script on every assistant turn. Fields relevant to fleet tracking:
- `model.id`, `model.display_name`
- `cwd`, `workspace.current_dir`, `workspace.project_dir`
- `cost.total_cost_usd`, `cost.total_duration_ms`,
  `cost.total_api_duration_ms`, `cost.total_lines_added/removed`
- `context_window.total_input_tokens`, `total_output_tokens`
- `context_window.used_percentage`, `remaining_percentage`,
  `context_window.context_window_size`
- `exceeds_200k_tokens`
- `effort.level`, `thinking.enabled`
- `rate_limits.five_hour.used_percentage`,
  `rate_limits.seven_day.used_percentage`

Statusline scripts run after every assistant message, after `/compact`,
on permission/vim mode change, and optionally on a `refreshInterval`
timer. Anthropic's note: "the status line runs locally and does not
consume API tokens." Stdout shows up in the UI, but a script can
side-effect anywhere — including writing JSON to a sentinel file.

### 1.2 Prior art — agent frameworks + community tools

LangGraph, AutoGen, and CrewAI all converge on the same pattern:
instrument the LLM-call boundary, ship spans to a collector
(Langfuse / MLflow / AgentOps / SigNoz / Langtrace), render in a
separate UI. The pattern doesn't transfer cleanly to claude-bus
because **we don't own the call site** — Claude Code does. The
transferable idea is narrower: a sidecar that snapshots
already-emitted state into a shared store.

Every community Claude Code dashboard I surveyed reads the same
source — transcript JSONLs under `~/.claude/projects/`:
`phuryn/claude-usage` (HTTP), `Maciek-roboblog/Claude-Code-Usage-Monitor`
(TUI, 3-sec refresh), `ccusage` (local CLI), `tokscale` (multi-CLI),
the VS Code "Claude Code Dashboard" extension. None invent new
instrumentation. Validation that the data is reachable.

### 1.4 Candidate designs for claude-bus

**Design A — statusline sidecar (recommended).**

Each agent's `claude` runs a shared statusline script that does two
things on every tick: (1) prints the normal status string, (2)
atomically writes the full JSON blob to
`$STATE/status/<agent>.json`. A new `bus track` viewer reads those
files and renders a fleet-wide table (or feeds `bus monitor`).

- Pros: zero broker changes; uses pre-computed numbers (`used_percentage`,
  `total_cost_usd`, rate-limits) that the transcript route can't give
  us without re-deriving; updates exactly when state changes (per
  turn); cwd already in the payload; consistent with the existing
  focus-as-presence sentinel pattern.
- Cons: requires changing `settings.json` to set `statusLine.command`
  — global per-clone, not per-agent. Need a way to make the sidecar
  cheap (atomic write, no fork-storm). Doesn't capture "what task is
  the agent on" — that's a different signal (see below).
- Effort: ~50 lines of shell or C++ for the sidecar + ~150 LOC for
  `bus track`.

**Design B — transcript scraper.**

`bus track` parses `~/.claude/projects/<uuid>.jsonl` per agent
(transcript_path from latest event for that agent). Sum
`message.usage.*` for cumulative tokens; tail for last turn.

- Pros: works retroactively (any past session); no new
  instrumentation; matches what every community tool does.
- Cons: re-reads files on every invocation (mitigated by mtime cache);
  doesn't give us `used_percentage` or `context_window_size` without
  duplicating Claude Code's pricing/sizing logic; doesn't see cost in
  USD; can lag a turn (transcript is written after the API response,
  hooks fire concurrently).
- Effort: ~200 LOC including a small JSON streamer.

**Design C — hook-emitted token events.**

A `Stop` hook tails the transcript_path it was given, finds the latest
assistant line, extracts `message.usage`, and appends a synthetic
`Tokens` record to events.jsonl. `bus track` reads events.jsonl only.

- Pros: keeps the existing "events.jsonl is the cross-agent view" frame
  (per `CLAUDE.md`); single source of truth.
- Cons: hooks already run on the hot path; tail-reading the transcript
  on every Stop is wasteful; race window because transcript writes
  and hook fires are independent; still misses cost / context_window
  data that only the statusline path has.
- Effort: ~30 lines of bash, but the data is a strict subset of A.

### 1.5 Focus — separate question

"What is this agent working on" is not derivable from tokens. Cheapest
signals first: (1) latest `UserPromptSubmit.prompt` from events.jsonl
— what task was just handed in; (2) latest
`Stop.last_assistant_message` — what the agent is currently saying;
(3) declarative `/focus "doing X"` sentinel — highest signal, requires
the agent to remember to update. Skip (3) for v1. Use (1) + (2) from
events.jsonl. No new mechanism.

### 1.6 Recommended path

Ship **Design A + signal 1 + signal 2**:
- `settings/statusline.sh` writes JSON to
  `$STATE/status/<agent>.json` (atomic rename).
- `.claude/settings.json` adds `statusLine.command` once.
- `bus track` reads every status file + tails events.jsonl for the
  latest `UserPromptSubmit.prompt` per agent + (optionally) latest
  `Stop.last_assistant_message`. Renders one row per agent.

This keeps the broker untouched (the file comms flagged), reuses
already-emitted state, and aligns with the focus-as-presence pattern
the fleet already runs. Drops gracefully if the statusline script
isn't installed — `bus track` just shows the events-derived columns.

---

## Thread 2 — does comms need its own CLAUDE.md?

### 2.1 What comms already loads

A `comms` session reads, in order:
1. Anthropic's bundled Claude Code system prompt.
2. `~/.claude/CLAUDE.md` (sulin's global).
3. `/home/sulin/claude-bus/CLAUDE.md` (project; all agents read this).
4. `/home/sulin/claude-bus/CLAUDE.local.md` (per-clone, gitignored;
   not yet wired but documented).
5. `roles/comms.md` — agent-style markdown with YAML frontmatter,
   loaded as the role definition (140 lines, tight, action-rules).
6. `docs/comms.md` — narrative (284 lines), the "why comms exists"
   essay. Loaded by reference, not auto-mounted.

Already a five-layer stack. Adding a sixth `CLAUDE.md` would compound
the "instruction file contradicting itself" failure mode the
agent-teams community calls out as the biggest single risk.

### 2.2 What's missing today

Reading `roles/comms.md` against actual orchestration friction we've
hit:

| Need | Covered today? |
|------|---------------|
| Approval gate around `bus msg mail` | yes, explicit |
| Discovery verbs (`bus agents`, `introduce`) | yes |
| Receive-then-summarize loop | yes |
| What NOT to do (no edits, no autonomous sends) | yes |
| **When to spawn a new agent vs. reuse one** | no |
| **Stuck-agent diagnosis playbook** | no |
| **Task tracking conventions (Tasks vs memory)** | no |
| **Multi-recipient dispatch heuristics** | no |
| **When to interrupt sulin vs. defer** | partial |

The gaps aren't "what comms is" — they're "how comms should think when
the situation is non-obvious." Exactly the orchestrator territory the
Anthropic multi-agent-research write-up describes as needing
"frameworks for collaboration, not rigid rules — defining division of
labor, problem-solving approaches, and effort budgets."

### 2.3 Prior art

- **Anthropic multi-agent research system blog** — orchestrator prompts
  focus on collaboration frameworks, not control flow. Teach delegation,
  not steps.
- **Claude Code Agent Teams** (official) — has explicit "delegate mode"
  that restricts the lead to coordination tools (mirrors how
  `roles/comms.md` already removes Edit/Write).
- **Claude Code subagent docs** — recommend a tight 30-60 line system
  prompt; "longer and you're duplicating what belongs in CLAUDE.md."
  `roles/comms.md` already pushes that ceiling at 140 lines.

### 2.4 Recommendation

**Do not add a new CLAUDE.md.** Adding a sixth instruction layer
multiplies the contradiction surface for negligible benefit — comms
already reads the project CLAUDE.md.

**Do add `docs/comms-playbook.md`**, written as a reference comms is
told to consult when the orchestrator decision is non-obvious. The
existing `roles/comms.md` adds one line pointing at it. Keep
`roles/comms.md` lean (it's the role spec); put thinking-in-context
material in the playbook.

Proposed skeleton (target 80-120 lines, scannable, references memory
slugs rather than restating):

- **When to spawn vs. reuse** — same-neighborhood task → reuse;
  context poisoned / wrong role / wedged → spawn. Check context-window
  % before piling on.
- **Stuck-agent diagnosis** — symptom matrix (WORKING-but-silent,
  NeedsInput, silent dropped turn, /clear pending) and the first
  action per symptom.
- **Multi-recipient dispatch** — broadcast vs. individual; convergence
  pattern (ask each agent to mail back; comms summarizes).
- **Task tracking** — TaskCreate in-conversation; bus msg mail across
  agents; memory for cross-session lessons only.
- **Interrupt vs. defer sulin** — interrupt on ambiguous intent /
  irreversible action / agent-only-sulin-can-answer; defer otherwise.
- **Anti-patterns** — unconfirmed `/clear`, mid-stream mail (see the
  `mid-stream silent dropped turn` memory), auto-reply.

### 2.5 What this changes for sulin

- One new file under `docs/`. Gitignore + license unchanged.
- One-line addition to `roles/comms.md` pointing at the playbook.
- No change to project `CLAUDE.md`.
- No new top-level instruction layer.

---

## Sources

Claude Code docs:
- [hooks](https://code.claude.com/docs/en/hooks) ·
  [statusline](https://code.claude.com/docs/en/statusline) ·
  [sub-agents](https://code.claude.com/docs/en/sub-agents) ·
  [Agent Teams](https://code.claude.com/docs/en/agent-teams)

Anthropic engineering:
- [Multi-agent research system](https://www.anthropic.com/engineering/multi-agent-research-system) ·
  [orchestrator-workers cookbook](https://github.com/anthropics/anthropic-cookbook/blob/main/patterns/agents/orchestrator_workers.ipynb)

Community usage tools (all read transcript JSONLs):
- [phuryn/claude-usage](https://github.com/phuryn/claude-usage) ·
  [Maciek-roboblog/Claude-Code-Usage-Monitor](https://github.com/Maciek-roboblog/Claude-Code-Usage-Monitor) ·
  [ccusage](https://github.com/ryoppippi/ccusage) ·
  [tokscale](https://github.com/junhoyeo/tokscale)

Framework observability:
- [LangGraph](https://www.langchain.com/articles/agent-observability) ·
  [AutoGen tracking](https://microsoft.github.io/autogen/0.2/docs/notebooks/agentchat_cost_token_tracking/) ·
  [CrewAI tracing](https://docs.crewai.com/en/observability/tracing)
