# CLAUDE.md conventions

How to write a CLAUDE.md that Claude actually follows. Synthesized from
Anthropic's official guidance (Claude Code memory page, best-practices
guide, prompt-engineering guide, prompt-caching docs), plus a sweep of
real-world examples. Primary sources cited inline; unverified claims
flagged.

CLAUDE.md loads into every session as a user message after the system
prompt. It competes with your conversation for context-window space and
for instruction-following budget. Treat it like code: keep it short,
keep it specific, keep it actionable.

## 1. Voice — imperative, specific, verifiable

Anthropic's prompt-engineering guide names the single highest-leverage
habit: *be clear and direct*. "Claude responds well to clear, explicit
instructions. Being specific about your desired output can help enhance
results." The golden rule: "Show your prompt to a colleague with minimal
context on the task and ask them to follow it. If they'd be confused,
Claude will be too." (`platform.claude.com/.../be-clear-and-direct`)

That cashes out as three rules for CLAUDE.md:

- **Imperative phrasing wins.** "Use 2-space indentation" beats "Format
  code properly." "Run `npm test` before committing" beats "Test your
  changes." The memory doc puts it bluntly: write specifically enough
  that the instruction is *verifiable*.
- **Active voice over passive.** Imperative is already active. Where
  descriptive prose is necessary, prefer "Hooks emit JSONL" to "JSONL
  is emitted by hooks." Active sentences name a subject doing a thing,
  which the model can act on.
- **State the affirmative, not the negative.** From the prompting
  guide: "Positive examples showing how Claude can communicate with the
  appropriate level of concision tend to be more effective than
  negative examples or instructions that tell the model what not to
  do."

Before/after drawn from our own `CLAUDE.md`:

| Before | After |
|---|---|
| "C++23 for the bus internals. New tools land as C++23 in `src/`..." | "Write new bus internals in C++23 under `src/`; build with `cmake --build build`." |
| "Layouts are the API. A new use case usually means a new layout in `layouts/`..." | "Add a new use case by writing a layout in `layouts/`. Don't grow flags on existing scripts." |
| "The 'bus' metaphor is literal: agents are independent processes..." | (cut — pure description, not instruction) |

## 2. Structure — short, scannable, hierarchical

Anthropic's memory doc: **"target under 200 lines per CLAUDE.md file.
Longer files consume more context and reduce adherence."**
(`code.claude.com/docs/en/memory`)

Three structural rules:

- **Under 200 lines.** Past that, "Claude ignores half of it because
  important rules get lost in the noise." (best-practices) If you grow
  past 200, split into `.claude/rules/` with `paths:` frontmatter so
  each rule loads only when relevant.
- **Headings + bullets, not paragraphs.** "Claude scans structure the
  same way readers do: organized sections are easier to follow than
  dense paragraphs." (memory) Use `##` for top-level categories; use
  `**bold lead.** prose` for individual rules — that pattern reads as
  a scannable index while still letting each entry breathe.
- **Group by what triggers the rule, not by topic.** A "Git workflow"
  section is fine because every commit triggers it. A "History of the
  repo" section probably isn't, because nothing triggers Claude to act
  on it.

## 3. What belongs, what doesn't

Anthropic publishes an explicit include/exclude table
(`code.claude.com/docs/en/best-practices`). Reproduced for reference:

| ✅ Include | ❌ Exclude |
|---|---|
| Bash commands Claude can't guess | Anything Claude can figure out by reading code |
| Code-style rules that differ from defaults | Standard language conventions Claude already knows |
| Testing instructions and preferred test runners | Detailed API documentation (link instead) |
| Repo etiquette (branch naming, PR conventions) | Information that changes frequently |
| Architectural decisions specific to this project | Long explanations or tutorials |
| Dev-environment quirks (required env vars) | File-by-file descriptions of the codebase |
| Common gotchas or non-obvious behaviors | Self-evident practices like "write clean code" |

The litmus test from the memory doc: **"For each line, ask: 'Would
removing this cause Claude to make mistakes?' If not, cut it."**

Other content that bloats without payoff:

- **Marketing prose.** "Our cutting-edge platform..." — Claude doesn't
  sell, it codes.
- **Status snapshots.** "v2 is in progress", "implementation began
  YYYY-MM-DD" — these decay; commit messages and PR history are the
  source of truth.
- **Full API references.** Link out instead.
- **Rules that must always fire.** Convert to actual hooks in
  `.claude/settings.json`. The memory doc is explicit: "If the
  instruction is something that must run at a specific point... write
  it as a hook instead. Hooks execute as shell commands at fixed
  lifecycle events and apply regardless of what Claude decides to do."

## 4. Context-window and cache economics

Anthropic's prompt-caching doc: cache reads cost roughly 10% of fresh
input tokens, with two TTLs (5-minute default, 1-hour at higher write
cost). The cache invalidates strictly: **"Changes at each level
invalidate that level and all subsequent levels."**
(`platform.claude.com/.../prompt-caching`)

How this lands for CLAUDE.md:

- **Every edit is a billable event.** When you change CLAUDE.md, the
  next session pays a cache write (1.25x base input cost for the 5-min
  TTL) instead of a read (0.1x). On a 200-line file this is small in
  absolute terms, but it compounds across teammates and worktrees.
- **Where the breakpoint sits matters.** The memory doc notes that
  CLAUDE.md is delivered "as a user message after the system prompt,
  not as part of the system prompt itself." That means CLAUDE.md churn
  doesn't necessarily invalidate the *system-prompt* cache — only the
  messages-level cache (if a breakpoint sits there). **Flag:** the
  exact breakpoint placement is Claude Code harness behavior I cannot
  verify from public docs. If precise numbers matter, measure with the
  `InstructionsLoaded` hook or `claude --debug`.
- **Implication.** Frequent CLAUDE.md edits are cheaper than the
  framing of "every edit invalidates the cache" suggests. The real
  ongoing cost is *bloat* — content that ships into every session for
  the lifetime of the file — not the per-edit miss.

## 5. Our CLAUDE.md, scored

`/home/sulin/claude-bus/CLAUDE.md` runs ~190 lines. Within budget on
size. Top violations against the conventions above:

### Violation 1 — "Commands" reproduces `bus help`

Lines 119–181 list every `bus` subcommand with a description. This is
discoverable: `bus help` prints it. Per the include/exclude table,
"anything Claude can figure out by reading code" → exclude.

**Proposed rewrite:**

> ## Commands
>
> Run `bus help` to discover subcommands. The broker daemon is `bus
> broker run` (singleton via flock on `$STATE/broker.pid`); everything
> else routes through `bin/bus <noun> <verb>`. Producers use
> `bus msg mail|slash|enqueue|broadcast`; consumers use `bus msg
> fetch|peek|body`. Direct TUI writes are `bus msg send`.

### Violation 2 — "Agent-to-agent comms / The broker" is documentation, not instruction

Lines 52–110 explain broker design (topic kinds, cursor semantics by
kind, reliability, retry escalation) as prose + a table. It's useful
context but reads as a tutorial, which the exclude column rules out.

**Proposed rewrite** (replace lines 52–110):

> ### The broker
>
> Queued/typed delivery routes through `bus broker run`. Topic kinds
> (`agent-inbox`, `tui-commands`, `work-queue`, `pubsub`, `blackboard`,
> `append-log`) and cursor semantics live in `docs/mechanics-reference.md`.
> Cliff notes: `bus msg mail AGENT body` enqueues to `inbox-AGENT`;
> `bus msg slash AGENT /cmd` enqueues a TUI command with
> `deliver_when=idle`.

### Violation 3 — Status-snapshot prose decays

Line 9 currently reads: "Implementation began 2026-05-24. `bin/` holds
the working bus primitives; `layouts/` has a bash demo and a
two-claude layout; `settings/` is still scaffolded."

These statements lose accuracy with every commit. `git log` and a
directory listing tell the same story without bit-rot risk.

**Proposed rewrite:** delete the sentence.

### Violation 4 — "Layouts are the API" is an observation, not an instruction

Current: "**Layouts are the API.** A new use case usually means a new
layout in `layouts/`, not new flags on existing scripts."

The bold lead is good; the body softens with "usually means". Make it
imperative:

**Proposed rewrite:**

> - **Layouts are the API.** Add a new use case by writing a layout in
>   `layouts/`. Don't grow flags on existing scripts.

### Violation 5 — "Conventions" mixes architecture with conventions

Current: "**C++23 for the bus internals.** New tools land as C++23 in
`src/`, built by CMake into `bin/`. Flat `bus::` namespace. The
reproducible dev env is `flake.nix` + `.envrc` (direnv); `nix develop`
drops you in if direnv isn't enabled. Existing shell scripts will be
rewritten as time permits."

This tells Claude what *is*, not what to *do*. The trailing sentence
("Existing shell scripts will be rewritten as time permits") is a
status snapshot that decays.

**Proposed rewrite:**

> - **Write C++23 in `src/`.** Build with `cmake --build build` inside
>   `nix develop`. Flat `bus::` namespace, `.h` headers (not `.hpp`).

(The decaying status sentence: cut.)

## 6. Voice checklist

Run each new or edited line through this list before adding it to
CLAUDE.md:

- [ ] Imperative voice? ("Do X" / "Use Y" / "Avoid Z")
- [ ] Names a concrete file, command, or invariant Claude can verify?
- [ ] Could a teammate with no context follow it without asking?
- [ ] Would Claude figure it out by reading the code? (If yes, cut.)
- [ ] Overlaps another rule? (Resolve before adding.)
- [ ] Will it still be true in three months? (If no, move to a CHANGELOG
  or a path-scoped rule.)
- [ ] Actionable — not descriptive? (If it reads as "this repo is..." or
  "implementation began...", move to README or delete.)
- [ ] Must it run *every* time? (If so, write a hook in
  `.claude/settings.json`, not a CLAUDE.md instruction.)

The overall heuristic from Anthropic's best-practices doc: **"Treat
CLAUDE.md like code: review it when things go wrong, prune it
regularly, and test changes by observing whether Claude's behavior
actually shifts."**

## Sources

- Anthropic, *How Claude remembers your project* —
  `https://code.claude.com/docs/en/memory`
- Anthropic, *Best practices for Claude Code* —
  `https://code.claude.com/docs/en/best-practices`
- Anthropic, *Prompting best practices: Be clear and direct* —
  `https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/be-clear-and-direct`
- Anthropic, *Prompt caching* —
  `https://platform.claude.com/docs/en/build-with-claude/prompt-caching`
- Andrej Karpathy + Forrest Chang viral CLAUDE.md (100K+ stars; the
  source of the four behavioral principles now living in our
  `## Code changes` section). Repo URL not directly verified from
  Anthropic-primary sources; treat as informational, not authoritative.
