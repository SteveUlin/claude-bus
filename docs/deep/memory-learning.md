# Long-Running Agents That Learn Over Time — A Deep Reference for claude-bus

> Written 2026-05-28. The deep companion to `docs/modern-agent-techniques.md` §3.2,
> for one topic only: **how a durable, pane-backed agent accumulates competence a
> headless fan-out structurally cannot.** Principle-first: every technique leads
> with the *mechanism / tradeoff / design pressure*, then the concrete code and
> the claude-bus mapping. Skim the tables; read the synthesis at the end.

claude-bus's agents are *durable*: each lives in its own zellij pane, resumes its
own session by `--continue`, owns its own jj workspace, and writes its own
transcript JSONL. A dynamic-workflow subagent evaporates when its run ends; a
claude-bus agent persists across days. **Continuity is the substrate for
learning, and learning over time is the one capability the headless-fan-out
corner of the design space cannot reach.** This document is about how to turn
that latent substrate — which you already have, mostly unused — into compounding
competence.

The central thesis, stated up front so the rest reads as elaboration:

> **You already capture the episodic trace (transcripts + `events.jsonl`). You
> have a global semantic store (auto-memory `MEMORY.md`). What you lack is the
> *reflection step that converts episodic → semantic/procedural*, a *per-agent*
> learning file, and any *injection* of that file at `SessionStart`. The gap is
> not storage. It is the distillation loop and the read-path.**

---

## Part 1 — The Core Principles & Tradeoffs

### 1.1 Why "learning without fine-tuning" is the only learning that fits

**The principle.** Fine-tuning bakes knowledge into weights: it is expensive,
slow (hours-to-days per cycle), destructive (catastrophic forgetting), and
opaque (you cannot read what changed). For an agent that must adapt *daily* to a
specific codebase, a specific human, a specific failure it just hit, weight
updates are the wrong granularity. The alternative is **in-context learning over
durable external memory**: the model's weights stay frozen; competence lives in
*files the model reads at runtime*. This is auditable (you can `cat` the
learnings), instantly editable (fix a wrong lesson by editing a line), reversible
(delete the file), and cheap (a few hundred tokens of context).

The tradeoff is honest: external memory consumes context budget on every turn it
is loaded, and it competes with the ~150-instruction practical ceiling that
`modern-agent-techniques.md` §4.1 documents. So the design pressure is **maximize
signal per token of injected memory** — distill, prune, rank-by-relevance — not
"store everything."

**The 2026 consensus on timescale.** Self-improving skill systems report *"five
to ten run cycles before clear behavioral changes"* (MindStudio). Learning over
time is a *slow compounding* return, not a one-shot. This argues for a low-effort,
low-risk, always-on loop rather than a heavyweight system you build once and
abandon.

### 1.2 The CoALA four-store model — name the memory you already have

**The principle.** The cleanest taxonomy of agent memory (CoALA, Sumers et al.)
splits memory by *what it is for*, and each store has a different read/write
cadence and a different home:

| Store | What it holds | Read cadence | Write cadence | Where in claude-bus today |
|---|---|---|---|---|
| **Working** | the live context window | every token | every turn | the running TUI session (ephemeral) |
| **Episodic** | what happened — raw interaction traces | rarely, on demand | continuously | **transcripts + `events.jsonl`** (have it) |
| **Semantic** | distilled facts — "how X works", "sulin prefers Y" | on task start | after reflection | **auto-memory `MEMORY.md`** (global, not per-agent) |
| **Procedural** | reusable skills / how-to procedures | on task match | when a procedure proves reusable | **`.claude/skills/`** (exists, not grown automatically) |

The deep insight: **these are not interchangeable, and the bugs come from
conflating them.** Episodic memory is cheap to write and expensive to read (you
don't load a day of transcripts into context). Semantic memory is expensive to
write (requires distillation) and cheap to read (a few lines). The *learning
mechanism* is precisely **the function that reads episodic and writes
semantic/procedural** — reflection. Everything in Part 2 is a variation on that
function.

### 1.3 The two failure modes of external memory: staleness and bloat

**The principle.** External memory rots in two opposite directions, and a healthy
system must counter both:

- **Staleness** — a lesson that *was* true becomes false (the API changed, the
  bug got fixed, sulin changed their mind). A stale lesson is worse than no
  lesson: the model trusts it. Counter: **every lesson must be falsifiable and
  prune-on-contradiction.** Your own `MEMORY.md` policy ("prune with equal
  aggression... delete memories that prove wrong") is exactly this instinct.
- **Bloat** — memory grows past the context budget; signal drowns in volume; the
  model starts dropping instructions (the ~150-rule ceiling). Counter:
  **consolidation + ranked retrieval.** Either compress periodically (your
  "consolidate at 200 lines" rule) or *retrieve* only the relevant subset per
  task (generative-agents, §2.3) rather than injecting all of it.

The Anthropic memory-tool prompting guidance encodes both: *"always try to keep
its content up-to-date, coherent and organized. You can rename or delete files
that are no longer relevant. Do not create new files unless necessary."* That is
anti-bloat (don't create files) + anti-staleness (delete the irrelevant) in two
sentences.

### 1.4 The durable-agent superpower, stated precisely

**The principle.** A headless fan-out subagent cannot learn over time because it
has no *over time* — it has a single bounded run. The claude-bus agent's
advantage is not "it's smarter"; it is **identity persistence + trace
persistence**. Concretely, three things must all hold for learning to accrue, and
claude-bus is the rare harness where all three do:

1. **Stable identity** — the same agent is the same agent across sessions
   (claude-bus: `--name NAME` + per-agent jj workspace + `--continue`).
2. **Durable trace** — what the agent did is recorded somewhere replayable
   (claude-bus: transcript JSONL + `events.jsonl`).
3. **A read-path back in** — distilled learnings re-enter the agent's context on
   the next run (claude-bus: **missing** — `SessionStart` does not inject
   anything; see Part 5).

Items 1 and 2 you have. Item 3 is the one structural gap, and it is the highest-
leverage fix in this entire document.

---

## Part 2 — How the Best Implementations Actually Do It

Dense reading of the canonical systems, with quoted source. Each illustrates a
different point in the design space; the synthesis (Part 4) combines them.

### 2.1 Reflexion — verbal reinforcement: failure → reflection text → next attempt

**The mechanism.** Reflexion (Shinn et al.) is the purest reflection loop. On a
failed attempt, the agent generates *natural-language self-criticism* and prepends
it to the next attempt's prompt. No weights move; the "policy update" is text in
context. From `programming_runs/reflexion.py`, the loop is literally:

```python
reflection = gen.self_reflection(cur_func_impl, cur_feedback, model)
reflections += [reflection]
# ... next attempt conditions on the accumulated reflections:
gen.func_impl(func_sig=item["prompt"], model=model, strategy="reflexion",
              prev_func_impl=cur_func_impl, feedback=cur_feedback,
              self_reflection=reflection)
```

**The principle worth stealing.** The reflection is generated *at the moment of
failure, with the failure's full context still live*, then persisted as compact
text. This is the cheapest possible learning step: one extra LLM call that reads
"what I just tried + why it failed" and emits "what I'll do differently."

**The flaw worth noting.** `reflections += [reflection]` has **no bound** — the
buffer grows one entry per iteration, capped only by `max_iters`. In a
single-task loop that's fine; in a *long-running agent over months*, an unbounded
reflection log is the bloat failure mode (1.3). claude-bus must add the bound
Reflexion omits: consolidate or rank, don't append forever.

### 2.2 Voyager — procedural memory as a growing, retrievable skill library

**The mechanism.** Voyager (Wang et al.) is the canonical *procedural* memory
system: it writes *successful solutions as reusable code*, indexed by a natural-
language description, and retrieves the relevant skills by embedding similarity
when a new task arrives. From `voyager/agents/skill.py`:

```python
# add_new_skill — store code + description, index the description:
self.skills[program_name] = {
    "code": program_code,
    "description": skill_description,
}
self.vectordb.add_texts(
    texts=[skill_description],
    ids=[program_name],
    metadatas=[{"name": program_name}],
)
```

```python
# retrieve_skills — similarity search the description vectors, return code:
k = min(self.vectordb._collection.count(), self.retrieval_top_k)  # default 5
docs_and_scores = self.vectordb.similarity_search_with_score(query, k=k)
```

The crucial design choice is **index on the description, store the code.** The
description is what the model writes when it solves something; it's generated by a
dedicated prompt (`voyager/prompts/skill.txt`):

> *"You are a helpful assistant that writes a description of the given function...
> Do not mention the function name... Try to summarize the function in no more
> than 6 sentences. Your response should be a single line of text."*

**The principles worth stealing.**
1. **Skills are *earned by success*, not declared.** Voyager only adds a skill
   after the task verifiably worked. This is the anti-staleness guarantee at write
   time — you don't memorize a procedure that didn't work.
2. **Retrieve by relevance, not load-everything.** The vectordb similarity search
   means the context only ever sees the ~5 skills relevant to *this* task, not the
   whole library. This is the anti-bloat answer: a growing library with bounded
   per-task injection.
3. **The description is the index *and* the disclosure.** Natural-language
   summary → embedding → retrieval. The same pattern Claude Code skills use
   (the `description` frontmatter triggers progressive disclosure).

### 2.3 Stanford generative agents — the memory stream + recency·importance·relevance retrieval

**The mechanism.** Generative agents (Park et al.) maintain a flat append-only
**memory stream** of observations and rank them for retrieval by a weighted sum of
three independently-normalized scores. From
`persona/cognitive_modules/retrieve.py`, `new_retrieve()`:

```python
# recency: exponential decay over chronological position
recency_vals = [persona.scratch.recency_decay ** i
                for i in range(1, len(nodes) + 1)]
# importance: an LLM-assigned "poignancy" score stored on the node
importance_out[node.node_id] = node.poignancy
# relevance: cosine similarity of node embedding to the query focal point
# ... all three normalized to [0,1] via normalize_dict_floats(d, 0, 1), then:
master_out[key] = (persona.scratch.recency_w  * recency_out[key]   * 0.5
                 + persona.scratch.relevance_w * relevance_out[key] * 2
                 + persona.scratch.importance_w * importance_out[key] * 3)
# top_highest_x_values(master_out, n_count)  # default n_count = 30
```

The weights `gw = [0.5, 3, 2]` (recency, importance, relevance) say something
designed: **importance and relevance dominate; recency is a tiebreaker.** A highly
relevant or important memory wins even if old; recency only breaks near-ties.

**The principles worth stealing.**
1. **Retrieval is a *ranking*, not a filter.** You don't ask "is this memory
   relevant yes/no"; you score every memory on three axes and take the top-N. This
   gracefully handles a large store.
2. **Importance is assigned at write time by the model itself** ("poignancy"). A
   memory's worth is a first-class field, not inferred at read time. This lets you
   keep a big stream cheaply and still surface the few high-value items.
3. **Reflection synthesizes higher-level memories from base observations** — the
   generative-agents "reflection" step periodically reads recent high-importance
   observations and writes derived insights *back into the same stream*, which are
   then themselves retrievable. Episodic → semantic, in one substrate.

### 2.4 Anthropic memory tool — the official client-side, file-backed memory primitive

**The mechanism.** The `memory_20250818` tool gives the model six commands over a
client-controlled `/memories` directory: **`view`, `create`, `str_replace`,
`insert`, `delete`, `rename`** (the same verb set as the text-editor tool, plus
rename). It is *client-side*: "Claude makes tool calls to perform memory
operations, and your application executes those operations locally," so you choose
the backend (file, DB, encrypted). The system prompt auto-injects a protocol:

> *"IMPORTANT: ALWAYS VIEW YOUR MEMORY DIRECTORY BEFORE DOING ANYTHING ELSE...
> As you make progress, record status / progress / thoughts etc in your memory.
> ASSUME INTERRUPTION: Your context window might be reset at any moment, so you
> risk losing any progress that is not recorded in your memory directory."*

**The principles worth stealing.**
1. **"Check memory first" as a hard protocol, not a hope.** The read-path is
   *mandated* before work begins. claude-bus has no equivalent — agents don't
   reliably read their own learnings at session start.
2. **"Assume interruption."** The model is told its context can vanish, so it
   must externalize state continuously. This is the durable-agent mindset made
   explicit to the model. A claude-bus agent that gets `/clear`'d or
   `/compact`'d should have internalized the same discipline.
3. **The multi-session software pattern**: an *initializer session* bootstraps a
   progress log + feature checklist; *subsequent sessions* open by reading them;
   *end-of-session* updates the progress log. *"Work on one feature at a time.
   Only mark a feature complete after end-to-end verification."* This is a concrete
   episodic-checkpoint discipline directly applicable to long-running fleet work.

### 2.5 Context editing + compaction — making room *for* memory by clearing context

**The mechanism.** The companion to the memory tool is **context editing**
(`clear_tool_uses_20250919`): when input tokens cross a threshold, old tool
results are stripped from the live context. Defaults from the docs:

```json
{ "type": "clear_tool_uses_20250919",
  "trigger":       { "type": "input_tokens", "value": 30000 },
  "keep":          { "type": "tool_uses",    "value": 3 },
  "clear_at_least":{ "type": "input_tokens", "value": 5000 },
  "exclude_tools": ["web_search"] }
```

The deep point Anthropic makes explicit: *"context is a finite resource with
diminishing returns, and irrelevant content degrades model focus."* Clearing isn't
just cost — it's *curation*. And the pairing with memory is the whole game: **clear
the bulky episodic detail from context, but persist the distilled lesson to
`/memories` first, so nothing load-bearing is lost in the clear.** Server-side
**compaction** does the same at the conversation level (summarize the whole
history near the window limit); *"memory persists important information across
compaction boundaries so that nothing critical is lost in the summary."*

**The principle worth stealing.** This is the read-path's mirror image: the
*write*-path should fire **before destructive context operations**, not after.
claude-bus's `PreCompact` hook is the natural trigger — distill-to-learnings
*before* the agent forgets. Today `PreCompact` only logs an event
(`settings/claude-settings.json` lines 92–102); it does no distillation.

### 2.6 The 2026 self-improving-skills practice (the pragmatic synthesis)

The blog-level practice that ties the academic systems to Claude Code reality
(MindStudio, Developers Digest):

- Maintain an external `learnings.md` injected at runtime; **the model never
  changes — the file does.**
- Update *incrementally* after each run; expect *"five to ten run cycles before
  clear behavioral changes."*
- **Prune aggressively**: the consensus is ~8–12 active skills; *"delete anything
  not triggered in 30 days."* (Anti-bloat, with a concrete TTL.)

This is your existing `MEMORY.md` discipline, generalized to skills and made
per-agent.

---

## Part 3 — The Design Space

Pulling the above into orthogonal axes, so the choices are explicit rather than
copied from whichever paper you read last.

### 3.1 Axis: write trigger — *when* does episodic become semantic?

| Trigger | Mechanism | Pro | Con | Fits claude-bus |
|---|---|---|---|---|
| **On failure** (Reflexion) | reflect when a task fails / agent wedges | cheap, targets real pain | misses lessons from successes | wedge/STUCK events are a natural trigger |
| **On success** (Voyager) | distill a working solution into a skill | earns procedural memory; anti-stale | needs a success signal | tie to verified task completion |
| **Periodic** (cron / generative-agents) | scheduled reflection over recent trace | catches slow patterns | may reflect on noise | nightly `claude -p` reflection job |
| **Pre-destruction** (context editing / `PreCompact`) | distill before context is cleared | nothing lost to compaction | only fires when context fills | wire the existing `PreCompact` hook |
| **Continuous** (memory-tool protocol) | model writes memory as it works | always fresh | costs tokens every turn; clutter risk | a `SessionStart`-injected discipline |

The honest read: **no single trigger is enough.** A robust system uses
*pre-destruction* + *periodic* for the bulk, with *on-failure* for the sharp
lessons. claude-bus should not pick one — it should wire the cheapest two first
(periodic cron + `PreCompact`) and add on-failure once the reducer (`modern-
agent-techniques.md` §1.2) makes failure detection reliable.

### 3.2 Axis: read path — *how* does semantic memory re-enter context?

| Read path | Mechanism | Pro | Con |
|---|---|---|---|
| **Inject-all at start** (your `MEMORY.md` today) | dump the whole file into context | simple; always present | bloats; hits the 150-rule ceiling |
| **`SessionStart` `additionalContext`** | hook emits learnings as a system reminder | documented, deterministic, per-agent | still inject-all unless ranked |
| **Retrieve-relevant** (Voyager/gen-agents) | embed query, fetch top-N skills/memories | scales to a big store; low per-task cost | needs an embedding index + a query |
| **Tool-pull on demand** (memory tool) | model calls `view`/`read` when it wants | model controls; minimal forced context | model may forget to check |

The design pressure: **inject-all is fine while small, retrieve-relevant is
necessary once large.** For claude-bus's near-term scale (a handful of agents, a
learnings file each), `SessionStart` inject-all is the right first step; graduate
to retrieve-relevant only when a learnings file outgrows ~100 lines.

### 3.3 Axis: granularity — *whose* memory, and at what scope?

This is the axis claude-bus gets *wrong by default today*, and it matters most.

| Scope | What it is | claude-bus status |
|---|---|---|
| **Global / project** | one store for the whole repo | auto-memory `MEMORY.md` — **this is all you have** |
| **Per-agent** | bast's lessons ≠ kvothe's lessons | **missing** — yet agents have distinct roles + workspaces |
| **Per-role** | all "coder"-kind agents share craft knowledge | **missing** — `roles/*.md` is static, doesn't accrete |
| **Per-task / episodic checkpoint** | progress log for one long task | **missing** — no resume-from-checkpoint discipline |

The principle: **memory scope should match identity scope.** claude-bus already
gives each agent a distinct identity (name, workspace, transcript, role). Lessons
bast learns about its codebase area are noise to kvothe. A single global
`MEMORY.md` either bloats with everyone's lessons or stays generic. The natural
fix is a **per-agent `learnings.md`** keyed by `$CLAUDE_BUS_AGENT_ID`, with an
optional **per-role** shared file for craft that generalizes across a kind.

### 3.4 Axis: substrate — *where* do the bits live?

| Substrate | Strength | Weakness | Use for |
|---|---|---|---|
| **Markdown file** (`learnings.md`) | human-readable, jj-versioned, grep-able, zero deps | no ranked retrieval; manual prune | semantic store, small scale |
| **Skill dir** (`.claude/skills/<n>/SKILL.md`) | progressive disclosure built in; auto-triggered | per-skill ceremony | procedural store |
| **Append-log** (your topic logs / `events.jsonl`) | crash-safe, replayable, audit | scan-to-read | episodic store (have it) |
| **Embedding index** (Voyager vectordb) | relevance retrieval at scale | a dependency; staleness of vectors | large memory, retrieve-relevant |
| **SQLite** | importance/recency fields + indexed query | a dependency | gen-agents-style scored retrieval |

The claude-bus-native answer: **Markdown for semantic, skill dirs for procedural,
your existing append-logs for episodic.** Don't reach for an embedding index until
a learnings file is too big to inject whole — and even then, prefer SQLite with
importance+recency columns (matches gen-agents, no embedding-model dependency,
fits your "SQLite for mutable coordination state" instinct from §2.4 of the broad
doc) over a vector DB.

---

## Part 4 — Novel Ideas Worth Considering

Ideas that fall out of *combining* the above with claude-bus's specific shape
(panes, broker, jj workspaces, transcripts). These are not in the prior doc.

### 4.1 Transcript-as-episodic-memory: the reflection corpus you already write

**The insight.** Each agent's transcript JSONL
(`~/.claude/projects/<encoded-cwd>/<uuid>.jsonl`) is a *complete, structured*
episodic record — `user`, `assistant`, `thinking`, `tool_use`, `tool_result`
records, timestamped, with `cwd` and `gitBranch`. The reflection pass doesn't need
to instrument anything new; it reads the transcript. The `thinking` blocks are
especially valuable — they contain the agent's *reasoning at decision points*,
which is exactly what a reflection step wants to critique. A nightly `claude -p`
job pointed at "yesterday's transcript for agent X" can emit "what wedged, what
worked, what I'd do differently" with zero new plumbing.

**Why claude-bus is uniquely positioned.** A headless fan-out has no persistent
transcript to reflect over; it's gone. Your per-agent workspace encoding
(`-home-sulin-claude-bus--workspaces-bast`) means each agent's transcripts are
*already namespaced* — the reflection job's input set is trivially selectable.

### 4.2 Reflection-as-a-bus-agent: a `librarian` role that curates the fleet's memory

**The insight.** Reflection is itself agent work, and claude-bus is a multi-agent
harness. Rather than a bare cron script, make reflection a **role**: a `librarian`
agent (coordinator-kind, like comms) that on a schedule reads each agent's recent
transcript + `events.jsonl`, distills per-agent learnings, *and* — uniquely —
spots cross-agent patterns ("bast and kvothe both hit the same jj-workspace
gotcha → promote to the shared role file"). It writes via the bus
(`bus msg mail bast "..."`) or directly to `learnings.md`, and escalates genuinely
novel failures to `inbox-human`. This unifies §3.4 (triage/self-healing) and §3.2
(reflection) of the broad doc into one durable role — and the human can *watch the
curation happen* in the librarian's pane, which a cron script hides.

### 4.3 Promotion ladder: episodic → per-agent → per-role → CLAUDE.md

**The insight.** Memory should *flow upward* as confidence rises, matching the
tiering in `modern-agent-techniques.md` §4.1:

```
transcript/events.jsonl   (episodic, raw, ephemeral-ish)
        │  reflection distills a candidate lesson
        ▼
learnings-<agent>.md      (per-agent semantic; falsifiable; pruned on contradiction)
        │  lesson recurs across sessions / proves stable
        ▼
roles/<role>.md           (per-role craft; injected via --append-system-prompt-file — you already do this!)
        │  lesson generalizes across roles / becomes inviolable
        ▼
CLAUDE.md / MEMORY.md     (project-wide rule; rare, high-signal)
```

The promotion criterion is **recurrence + survival**: a lesson earns the next tier
by appearing repeatedly and never being contradicted. This is the anti-bloat *and*
anti-staleness mechanism unified into one rule — and the bottom tier (per-agent)
absorbs the churn so the top tier (CLAUDE.md) stays under the 150-line ceiling.
Note the read-path for `roles/*.md` **already exists** (`agent-launch` strips
frontmatter and passes `--append-system-prompt-file`); the missing piece is making
that file *accrete* rather than stay static.

### 4.4 Importance-scored learnings, gen-agents style, in plain SQLite

**The insight.** When a per-agent learnings file outgrows inject-all, don't reach
for embeddings — adopt gen-agents' three-axis score but drop the cosine-similarity
relevance term (you have no query at `SessionStart`) and keep
**importance × recency**: each lesson row gets a model-assigned `poignancy` at
write time and a `last_confirmed` timestamp; `SessionStart` injects the top-N by
`importance_w·poignancy + recency_w·decay(last_confirmed)`. This is a ~30-line
SQLite table, no embedding model, and it directly counters both failure modes:
recency decay surfaces fresh lessons, importance keeps the load-bearing ones,
top-N caps the injected token cost.

### 4.5 `PreCompact` as the write-trigger — distill before you forget

**The insight.** The single cheapest place to fire reflection is the moment Claude
Code is *about to* compact, because that's exactly when episodic detail is about
to be lost and the agent is mid-task (so the lessons are sharp). Anthropic's own
memory+context-editing pairing is built on this principle (2.5). claude-bus's
`PreCompact` hook currently only logs. Wiring it to "append the last N turns'
key decisions to `learnings-<agent>.md`" turns a lossy compaction into a
checkpoint. This is the on-the-agent's-own-timeline complement to the nightly
cron (4.2) — it catches lessons *within* a long session, not just across sessions.

### 4.6 Memory as a bus topic — `blackboard` for fleet-shared semantic state

**The insight.** You already have a `blackboard` topic kind (last-value-wins,
non-destructive reads). Cross-agent *semantic* state — "the current build is
broken on main", "we decided to use the FIFO transport" — is exactly a blackboard
cell. Rather than each agent rediscovering it, the librarian (4.2) writes it to a
`fleet-knowledge` blackboard, and `SessionStart` injects the current value. This
makes the fleet's *shared* semantic memory reactive and durable without a new
mechanism — and `modern-agent-techniques.md` §2.5's "blackboard triggers" idea
becomes the change-notification path.

---

## Part 5 — How This Maps to claude-bus

This is the prescriptive section. Each item leads with the principle, then the
concrete change with file refs, then rough effort/payoff. Ordered by payoff÷effort.

### 5.1 Flaws / gaps in the current code

| # | Area | Issue | File ref |
|---|---|---|---|
| F1 | **No read-path** | `SessionStart` runs `log-event.sh` + `agent-register.sh` only. Neither emits `hookSpecificOutput.additionalContext`. **No learned memory ever re-enters an agent's context.** This is the structural gap — items 1+2 of §1.4 hold, item 3 does not. | `settings/claude-settings.json:51-65`, `settings/hooks/agent-register.sh` |
| F2 | **No reflection step** | The episodic trace (transcripts, `events.jsonl`) is captured but *never distilled*. There is no episodic→semantic function anywhere in `src/` or `settings/`. | (absence) `settings/hooks/`, `src/` |
| F3 | **Memory scope mismatch** | The only semantic store is the *global* per-project auto-memory `MEMORY.md`. Agents have distinct identities (name, workspace, role) but share one undifferentiated memory — so it must either bloat or stay generic (§3.3). | `~/.claude/projects/-home-sulin-claude-bus/memory/MEMORY.md` |
| F4 | **`PreCompact` is inert** | The hook fires and only logs an event; it does no pre-destruction distillation, missing the cheapest write-trigger (§2.5, §4.5). | `settings/claude-settings.json:92-102` |
| F5 | **Roles are static** | `roles/*.md` is injected via `--append-system-prompt-file` (`bin/agent-launch:194-210`) — a working read-path! — but nothing ever *writes* to those files, so per-role craft can't accrete (§4.3). | `bin/agent-launch:194-210` |
| F6 | **Reflexion-style unboundedness risk** | Any naive "append a lesson per session" will reproduce Reflexion's unbounded `reflections += [...]` bug (§2.1) at the scale of months. Must design the prune/consolidate path *with* the write path, not after. | (design) |

### 5.2 Recommendations, principle-first

**R1 — `SessionStart` injects per-agent `learnings.md` via `additionalContext`.**
*Principle:* learning over time requires a read-path back into context (§1.4 item
3); `additionalContext` is *"wrapped in a system reminder and inserted into the
conversation"* — the documented, deterministic mechanism. *Concretely:* add a
hook script that, on `SessionStart`, reads `$STATE/learnings/$CLAUDE_BUS_AGENT_ID.md`
(if present) and prints `{"hookSpecificOutput":{"hookEventName":"SessionStart",
"additionalContext": <file contents>}}`. This is the single change that activates
everything else — without it, distilled lessons are write-only.
**Effort: Low. Payoff: Very High.** (Closes F1, F3's read side.)

**R2 — Nightly reflection cron → per-agent `learnings.md`.**
*Principle:* the learning mechanism is the episodic→semantic distillation function
(§1.2); the cheapest robust trigger is periodic (§3.1). *Concretely:* a scheduled
headless `claude -p` job per agent that reads the day's transcript JSONL
(namespaced by the agent's workspace-encoded project dir — §4.1) + that agent's
`events.jsonl` slice, and **appends a small set of falsifiable lessons** to
`learnings-<agent>.md`, *then consolidates if over a line budget* (countering F6).
Headless is unambiguously right here — no human watches a 3am reflection.
**Effort: Med. Payoff: High.** (Closes F2.)

**R3 — Make reflection a `librarian` bus role, not a bare cron.**
*Principle:* reflection is agent work, and a durable role makes it watchable and
lets it spot cross-agent patterns (§4.2). *Concretely:* `roles/librarian.md` +
a scheduled prompt; it distills per-agent learnings, promotes recurring lessons
to `roles/<role>.md` (§4.3, exploiting the F5 read-path), writes fleet-shared
facts to a `fleet-knowledge` blackboard (§4.6), and escalates novel failures to
`inbox-human`. Unifies reflection + triage. *Tradeoff:* heavier than R2 — do R2
first, graduate to R3 if the cron proves valuable. **Effort: Med-High. Payoff:
High.**

**R4 — Wire `PreCompact` to checkpoint into `learnings.md`.**
*Principle:* distill before destruction — the moment before a compact is when
episodic detail is richest and about to be lost (§2.5, §4.5). *Concretely:*
replace the inert `PreCompact` log-only hook with one that captures the session's
key decisions/modified-files into the learnings file (or a per-session checkpoint
file the next session reads). *Tradeoff:* a hook doing an LLM distillation would
violate the "hooks must be fast <100ms" rule (§4.2 of broad doc) — so the hook
should only *snapshot the trigger + recent turns to a file*, and let R2's cron do
the LLM distillation later. **Effort: Low-Med. Payoff: Med.** (Closes F4.)

**R5 — Adopt the memory-tool "check memory first / assume interruption" protocol
in the agent prompt.**
*Principle:* the read-path must be *mandated*, not hoped for (§2.4); and an agent
that knows its context can vanish externalizes state continuously. *Concretely:*
add to the shared agent prompt / role files a short protocol: "At session start,
read your learnings. As you work, when you discover a durable lesson or your
context may compact, record it." This is the human-typed-TUI analog of the
memory-tool system prompt. **Effort: Low. Payoff: Med.**

**R6 — Define the promotion ladder + prune policy as the write contract.**
*Principle:* counter staleness (prune-on-contradiction) and bloat (consolidate +
promote) *with* the write path, not after (§1.3, §3.3, §4.3, F6). *Concretely:*
codify in the librarian/cron prompt: lessons are falsifiable one-liners with a
`last_confirmed` date; a lesson contradicted by newer evidence is deleted; a
lesson surviving N sessions is promoted up the ladder; a per-agent file over ~100
lines is consolidated. Mirror your existing `MEMORY.md` policy. **Effort: Low (it's
a prompt + a convention). Payoff: Med — but it's load-bearing for R2/R3 not
rotting.**

**R7 — Graduate to importance×recency SQLite retrieval *only* when needed.**
*Principle:* inject-all is fine small, retrieve-relevant is necessary large
(§3.2); but prefer scored-SQLite over embeddings when you have no query (§4.4).
*Concretely:* if a learnings file outgrows inject-all, move to a `lessons` SQLite
table with `poignancy` + `last_confirmed`, inject top-N by importance×recency.
*Do not do this preemptively* — it's complexity you won't recover at current
scale. **Effort: Med. Payoff: Low now / High later.**

### 5.3 What to deliberately NOT do

- **Don't fine-tune.** The whole topic is learning *without* weight updates;
  fine-tuning is the wrong granularity, cadence, and reversibility (§1.1).
- **Don't add an embedding/vector DB yet.** At a-handful-of-agents scale,
  inject-all (R1) and scored-SQLite-if-needed (R7) dominate. A vector store adds a
  model dependency and *its own* staleness problem (vectors drift from edited
  files) for retrieval quality you don't yet need.
- **Don't make the global `MEMORY.md` the per-agent store.** Scope memory to
  identity (§3.3); conflating them reproduces the bloat-or-generic dilemma.
- **Don't put LLM work in a synchronous hook.** `PreCompact`/`SessionStart` hooks
  block the agent; they may only do file I/O (snapshot/inject), never a model call
  (§4.2 of broad doc, R4 tradeoff).
- **Don't append reflections unboundedly.** Reflexion's `reflections += [...]` is
  fine for a bounded loop, fatal for a months-long agent (§2.1, F6). Design the
  prune path first.

---

## Part 6 — One-Paragraph Synthesis

claude-bus's durable, identity-stable, transcript-writing agents are the rare
harness where learning over time is even *possible* — the headless-fan-out corner
of the industry structurally cannot do it. You already have two of the three
required pieces: stable identity and a durable episodic trace. The missing third
is a **read-path** (`SessionStart` injecting a per-agent `learnings.md` via
`additionalContext` — R1, the single highest-leverage fix) and the **distillation
loop** that fills it (a nightly reflection cron, R2, ideally elevated to a
watchable `librarian` role, R3). Steal Reflexion's reflect-on-failure, Voyager's
earn-skills-by-success-and-retrieve-by-relevance, generative-agents'
importance×recency ranking, and the Anthropic memory tool's "check memory first /
assume interruption" protocol — but scope memory to identity (per-agent →
per-role → CLAUDE.md promotion ladder), bound it against staleness and bloat from
the start, and keep the substrate boring (Markdown + your existing append-logs +
skill dirs) until scale forces SQLite-scored retrieval. The payoff compounds
slowly — five-to-ten cycles before visible change — which is exactly why the right
move is the low-effort always-on loop, not a heavyweight system.

---

## Sources

**Memory & learning systems (source-read)**
- Voyager — `voyager/agents/skill.py` (`add_new_skill`, `retrieve_skills`), `voyager/prompts/skill.txt` (skill-description prompt). https://github.com/MineDojo/Voyager
- Reflexion — `programming_runs/reflexion.py` (`self_reflection`, `reflections` buffer). https://github.com/noahshinn/reflexion
- Stanford Generative Agents — `persona/cognitive_modules/retrieve.py` (`new_retrieve`, recency·importance·relevance scoring, `gw=[0.5,3,2]`). https://github.com/joonspk-research/generative_agents
- CoALA: Cognitive Architectures for Language Agents (working/episodic/semantic/procedural taxonomy). https://arxiv.org/pdf/2309.02427

**Anthropic primary surfaces**
- Memory tool (`memory_20250818`; `view`/`create`/`str_replace`/`insert`/`delete`/`rename`; `/memories`; check-first/assume-interruption protocol; multi-session pattern). https://platform.claude.com/docs/en/agents-and-tools/tool-use/memory-tool
- Context editing (`clear_tool_uses_20250919`; trigger 30k / keep 3 tool_uses / clear_at_least 5k; compaction pairing). https://platform.claude.com/docs/en/build-with-claude/context-editing
- Effective context engineering for AI agents. https://www.anthropic.com/engineering/effective-context-engineering-for-ai-agents
- Effective harnesses for long-running agents. https://www.anthropic.com/engineering/effective-harnesses-for-long-running-agents
- Claude Code hooks reference (`SessionStart` `additionalContext`, `PreCompact`). https://code.claude.com/docs/en/hooks

**2026 self-improving-skills practice**
- Self-improving Claude Code skills (external `learnings.md`, 5–10 cycles). https://www.mindstudio.ai/blog/self-improving-ai-skills-claude-code
- Best Claude Code skills 2026 (8–12 skills, 30-day prune). https://www.developersdigest.tech/blog/best-claude-code-skills-2026

**claude-bus source (this repo)**
- `bin/agent-launch` (identity, `--continue` resume, `--append-system-prompt-file` role read-path).
- `settings/claude-settings.json` (hook wiring; `SessionStart`, `PreCompact`).
- `settings/hooks/agent-register.sh`, `settings/hooks/log-event.sh` (episodic capture).
- `docs/modern-agent-techniques.md` §3.2, §4.1, §4.2 (the broad survey this extends).
</content>
</invoke>
