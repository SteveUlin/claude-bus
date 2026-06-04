# Modern Agent Engineering — A Reference for claude-bus

> **FROZEN — pre-refactor research (Phase-4 doc cleanup, 2026-06-03).**
> Broad prior-art survey, cited by-section across the frozen `deep/`
> references. Retained with that corpus rather than folded into
> `docs/prior-art.md` (folding would orphan those citations); re-evaluated
> after the broker-seam refactor (Phase 2) lands. Historical context, not
> current truth.

> Written 2026-05-28. A learning artifact and a backlog. Every technique leads
> with the **principle** (the mechanism, the tradeoff, the design pressure),
> then the concrete mapping to claude-bus. Read it for the *why*; the *what to
> build* falls out of that. Skim the tables, then read the synthesis at the end.

claude-bus already occupies a specific, defensible point in the design space:
**durable, pane-backed, human-attachable agents on a single host, coordinated by
a broker that injects text into live TUIs.** Most of the industry is sprinting
toward the *opposite* corner — ephemeral, headless, cloud-fanned-out subagents
whose state lives in a script, not a terminal. Anthropic shipped exactly that on
the day this was written (dynamic workflows, below). The right move is **not** to
chase that corner. It is to (a) steal the resiliency techniques that are corner-
agnostic, and (b) sharpen the one thing claude-bus is uniquely good at: a human
sitting *inside* a live multi-agent session, able to grab any keyboard.

The single highest-leverage realization in this document: **the flock'd TTY
write is the load-bearing weak point, and there is a strictly better transport
sitting unused in your stack — the headless `claude -p --input-format stream-json`
JSON-lines channel.** Everything else is incremental; that is structural.

---

## Category 1 — Base / Infrastructure

The broker is a C++23 daemon doing a 250 ms poll loop, regex-scanning an event
log to infer agent state, and writing bytes into PTYs. Three structural
weaknesses follow from that shape: polling latency, heuristic state inference,
and TTY contention. Each has a principled fix.

### 1.1 Text injection: the flock'd TTY write is the wrong primitive

**The principle.** A TUI like Claude Code owns its terminal: it controls the
cursor, the input line editor, bracketed-paste state, scroll region, and modal
overlays. When you `write()` bytes to the PTY master, you are *racing the
program's own reads* and *racing the human's keystrokes* through the same slave
fd. The kernel PTY guarantees ordering of bytes but **nothing about
interleaving semantics** — your "Enter" can land mid-paste, your text can land
while a modal is up, and `pty(7)` explicitly warns that "there may be a small
processing delay between a write to the master and the effect being visible at
the slave" ([pty(7)](https://www.man7.org/linux/man-pages/man7/pty.7.html)). This
is the same fragility tmux-orchestrator papers over with `send-keys; sleep 0.5`.
Bracketed-paste loss is a known, recurring TUI failure: when it's lost,
Shift+Enter submits instead of inserting a newline and multi-line text submits
line-by-line ([openclaw#18809](https://github.com/openclaw/openclaw/issues/18809),
[pi#2704](https://github.com/earendil-works/pi/issues/2704)). Your `sendToPaneSafe`
+ presence-sentinel logic is a *good* mitigation of an *inherently racy*
channel. The honest framing: you are reverse-engineering a private input
protocol over a shared bus.

**The strictly better answer — feed claude, not the terminal.** Claude Code
exposes a first-class programmatic input channel that does not touch the TTY at
all: `claude -p --input-format stream-json --output-format stream-json`. Each
side speaks newline-delimited JSON (NDJSON); you send user-message objects on
stdin and read assistant/result/init objects on stdout
([claude-code#24594](https://github.com/anthropics/claude-code/issues/24594),
[SDK streaming](https://code.claude.com/docs/en/agent-sdk/streaming-output)). The
Agent SDK is a thin wrapper over exactly this — "the SDK and CLI communicate
using a JSON-lines stream… multiplex control requests via request IDs"
([buildwithaws](https://buildwithaws.substack.com/p/inside-the-claude-agent-sdk-from)).
This is a *structured, framed, race-free, ACK-able* channel. There is no human
keyboard contention because the human isn't typing into a JSON pipe; there is no
bracketed-paste problem because there is no line editor; delivery is confirmed by
a `result` message, not inferred from a hook.

**The catch, and why you don't rip out the TTY path.** The whole point of
claude-bus is that a human can *attach to the pane and take over the keyboard*.
A headless `claude -p` process has no interactive TUI to attach to. So the two
modes are in genuine tension: the interactive TUI gives you human-attachability
and costs you a clean input channel; the headless JSON pipe gives you a clean
channel and costs you the pane. **The synthesis is a hybrid**, and it's the
single most important architectural idea here:

- Keep the interactive `claude --name` TUI in a zellij pane as the **human
  surface**. Human types here; this is your differentiator. Don't lose it.
- Run delivery through a **per-agent control FIFO that the agent itself drains**,
  not through TTY injection. Concretely: a `SessionStart`/`PreToolUse`-adjacent
  loop, or a tiny sidecar, reads framed messages from `$STATE/ctrl/<agent>.fifo`
  and the agent injects them as turns. This removes the broker→TTY write
  entirely; the broker writes to a FIFO/socket, the *agent* decides when to
  consume. (See 1.2 for the mechanism that makes "decides when" robust.)

| Injection mechanism | Race-free? | Human-attachable? | ACK quality | Verdict for claude-bus |
|---|---|---|---|---|
| flock'd TTY `write` (today) | No | Yes | inferred from hooks | Keep only as the *human-typing* path |
| tmux/zellij `send-keys` | No (worse) | Yes | none | Strictly worse than what you have |
| `TIOCSTI` ioctl | N/A | — | — | **Dead.** Disabled by default since Linux 6.2 (`dev.tty.legacy_tiocsti`); security hole. Do not use. |
| `expect`/PTY master fd | No | partial | pattern-match | Same race class as TTY write |
| **`claude -p` stream-json** | **Yes** | No (headless) | `result` message | The robust channel; pairs with a FIFO drain |
| Named FIFO drained *by the agent* | **Yes** | **Yes** | UserPromptSubmit | **Recommended hybrid** |

> **Skeptic's note.** Don't overcorrect into "rewrite claude-bus on the Agent
> SDK." That throws away human-attachability, which is the reason this project
> exists rather than being `amux`. The FIFO-drained-by-agent pattern keeps the
> pane *and* kills the race. Prototype it on one agent before committing.

### 1.2 State inference: replace regex-over-events with an event-sourced state machine

**The principle.** You currently derive IDLE/WORKING/STUCK/etc. by
regex/heuristic scanning of `events.jsonl` + pane dumps + transcript token
counts. Regex over a semi-structured stream is *brittle by construction*: it
couples your state logic to incidental text formatting, has no notion of "I
already consumed up to offset N," and silently misfires when the upstream format
drifts (your own memory notes a "mid-stream silent dropped turn" and a "broker
delivery wedge" — both are state-inference failures). The robust pattern is
**event sourcing**: treat `events.jsonl` as an append-only log of *facts*, and
derive state by folding a **typed reducer** over events past a stored cursor.
State becomes a pure function of `(prior_state, next_event)`; it is replayable,
testable in isolation, and impossible to get into an impossible state if the
transitions are a real state machine ([event sourcing with
SQLite](https://www.sqliteforum.com/p/event-sourcing-with-sqlite)).

**The deeper principle — derive state from *typed hook fields*, not text.**
Claude Code hooks emit structured JSON with stable field names. You don't need
to regex "Compacting…" out of a pane dump; `PreCompact` fires as an event, and
`UserPromptSubmit`/`Stop`/`PostToolUse` carry `tool_name`, `source`, etc.
([hooks reference](https://code.claude.com/docs/en/hooks)). Your memory already
records that "Compacting/NeedsInput/BootStuck come from events.jsonl payload
fields + pane mode" — formalize that into one reducer instead of scattered
scans. A pane dump should be a *last-resort tiebreaker*, not a primary signal.

**Map to claude-bus.**
- Define `enum class AgentState` and a single `Reduce(State, const Event&)`
  free function in the `bus::` namespace. Drive it from a parsed `Event`
  struct, not `std::regex` over lines.
- Store one cursor per consumer of the event log (you already do this for
  topics — apply the same discipline to state derivation).
- Make state transitions *total*: every (state, event) pair maps somewhere, even
  if to "unchanged." Add a table-driven test: feed canned event sequences,
  assert the resulting state. This turns "mid-stream dropped turn" from a
  production mystery into a failing unit test.

| Approach | Testable? | Survives format drift? | Effort | Payoff |
|---|---|---|---|---|
| Regex over events + pane dumps (today) | Hard | No | — | — |
| Typed reducer over parsed hook events | Yes (table tests) | Yes | Medium | High |

### 1.3 The poll loop: epoll/io_uring over a 250 ms sleep

**The principle.** A 250 ms poll loop trades latency for simplicity, and burns
CPU scanning topics that didn't change. The reactor pattern inverts this: block
in `epoll_wait`/`io_uring` until *something* (an inotify event on a topic log, a
timer fd for retries, a connection on the broker socket, readability on a control
FIFO) is actually ready, then handle exactly that. "Keep the event loop small
and deterministic… on a CQE, look up the connection, advance the state machine,
re-arm" ([event loop notes](https://iafisher.com/notes/2025/10/epoll-io-uring)).
io_uring further amortizes syscall cost by batching submissions/completions
([cor3ntin](https://cor3ntin.github.io/posts/iouring/)), but **for a single-host
broker with a handful of agents, epoll + `inotify` + `timerfd` is the right
level** — io_uring is a premature optimization here and adds real complexity.

**Map to claude-bus.** Replace `sleep(250ms)` with `epoll_wait` over:
`inotify` watches on `$STATE/topics/*.log` (delivery wakes on actual writes),
`timerfd`s for retry/ACK-timeout deadlines (one per in-flight message, or a
single min-heap timer), the broker `AF_UNIX` socket, and the per-agent control
FIFOs. Result: sub-millisecond delivery latency *and* near-zero idle CPU. This
also makes the "delivery loop silently stopped" wedge in your memory far less
likely, because there's no loop body that can get stuck — there are discrete
handlers.

> **Skeptic's note.** Don't do this *first*. The reducer (1.2) and the FIFO
> channel (1.1) buy you correctness. The event loop buys you latency and
> elegance. Sequence correctness before performance.

### 1.4 Parsing & robustness craft

- **One JSON parser, structured.** Pick a single header-only parser
  (simdjson for read-heavy hot paths, or nlohmann for ergonomics) and parse
  events into typed structs at the boundary. Never `grep`/regex JSON. This is
  the same principle as 1.2 applied at the byte level.
- **Advisory-lock pitfalls.** `flock` is *advisory* and *per-open-file-
  description* — it does not protect against a writer that doesn't take the
  lock, and it's released on the last close of *any* duplicated fd. Document the
  invariant ("every TTY writer MUST flock") because the kernel won't enforce it.
  Better: eliminate the shared-writer problem entirely (1.1).
- **Structured logging.** Emit the broker's own logs as JSONL with a stable
  schema (ts, level, agent, msg_id, event). You already have `events.jsonl` for
  agents; give the *broker* the same treatment so its decisions are replayable.

---

## Category 2 — Communication Patterns

The broker is already a respectable local message bus: typed topics, cursors,
in-flight tracking, retry-then-escalate. The gaps are in *delivery guarantees*
and *transport*, not in the topic model.

### 2.1 At-least-once + idempotency keys (you're closer than you think)

**The principle.** Any retrying delivery system is *at-least-once* by nature:
if an ACK is lost, you redeliver, and the recipient may see a message twice.
"Exactly-once" is a myth at the transport layer; the achievable target is
**at-least-once delivery + idempotent processing**. The mechanism is an
**idempotency key**: a stable per-message ID the consumer records; a redelivery
with a seen key is dropped ([sql-event-store dedup](https://github.com/mattbishop/sql-event-store)).
Your retry-3x-then-escalate is correct at-least-once; what's missing is the
consumer-side dedup so a retried message doesn't produce a duplicate *turn*.

**Map to claude-bus.** Your records already have `msg_id`. Have the agent-side
drain (1.1) record the last-seen `msg_id` per topic and refuse to re-inject a
seen id. This closes the loop: broker can redeliver freely (safe), agent never
double-acts. This directly hardens the "retried delivery interleaves" failure
mode.

### 2.2 Dead-letter queue — you have one, name it as such

**The principle.** A dead-letter queue (DLQ) is where messages go when delivery
*can't* succeed, so the main queue keeps draining instead of head-of-line
blocking. The pattern: N retries → move to DLQ → advance cursor → alert a human.
**You already built this**: "retries up to 3 attempts, then escalates: appends a
record to the `audit` topic and mails `inbox-human`… then advances the cursor."
That *is* a DLQ + alerting. The refinement is to make the DLQ **re-drivable**:
a human (or a triage agent, 3.4) should be able to inspect `audit`, fix the
cause, and *replay* the dead message rather than only reading its obituary.

**Map to claude-bus.** Add `bus msg redrive <msg_id>` that re-enqueues a
dead-lettered record to its original topic with a fresh attempt count. Cheap;
turns escalation from a dead end into a loop.

### 2.3 Backpressure

**The principle.** A producer that outruns a consumer either drops messages,
blocks the producer, or grows an unbounded queue (eventually OOM). A bounded
queue + an explicit policy (block / drop-oldest / reject) is the only honest
design. Your `--max-bytes` per topic is the right hook; the question is the
*policy* when full.

**Map to claude-bus.** For `agent-inbox`, the natural backpressure is **the
agent's own pace**: don't dispatch message N+1 until N is ACKed (you largely do
this via the in-flight gate). Make the policy explicit per kind: inbox =
one-in-flight (serialize), work-queue = bounded with reject-on-full (producers
retry), blackboard = drop-all-but-latest (already the semantics). Document it in
the topic-kind table.

### 2.4 Transport: Unix domain socket vs SQLite-WAL vs append-log

**The principle.** Three local-bus substrates, three tradeoffs:

| Substrate | Strength | Weakness | Fits claude-bus where |
|---|---|---|---|
| `AF_UNIX` socket (you use this for RPC) | low latency, natural request/reply, connection = liveness | no durability; restart loses in-flight | control plane (CLI↔broker), already correct |
| Append-only log file (you use this for topics) | durable, replayable, trivially auditable, crash-safe with O_APPEND | no indexed query; readers scan | topic logs, event sourcing — keep |
| **SQLite in WAL mode** | ACID, concurrent readers + 1 writer, atomic CAS, indexed queries, single file | a dependency; not a stream | **task claiming + cursors + in-flight** |

**The SQLite case, specifically.** amux uses **atomic task claiming via SQLite
CAS (compare-and-swap)** so "no two agents pick up the same ticket"
([amux](https://github.com/mixpeek/amux)). This is the textbook fix for the
multi-consumer `work-queue` race: instead of "fetch advances a cursor" (which is
racy across processes unless the broker serializes every fetch), a worker does
`UPDATE tasks SET owner=? WHERE id=? AND owner IS NULL` and checks
`rows_affected == 1`. The DB enforces single-assignment atomically. Your broker
*does* serialize, so you may not need this — but if work-queue consumers ever
pull *without* round-tripping the broker, SQLite CAS is the principled answer.
SQLite-WAL also gives you crash-safe cursors and in-flight tracking for free
(survives broker restart, unlike socket state).

> **Skeptic's note.** Don't SQLite-ify the *topic logs* — append-only files are
> already crash-safe, replayable, and grep-able, which is exactly what an audit
> bus wants. SQLite earns its place for *mutable coordination state* (claims,
> cursors, in-flight), not the immutable event stream. Use both, for what each
> is good at. This is the "dual-contract event store" pattern
> ([Medium](https://medium.com/@impactarchitecture/persistence-model-for-a-dual-contract-event-store-in-sqlite-53f3505f7d21)).

### 2.5 Blackboard — you have it; the missing piece is *triggers*

**The principle.** A blackboard is a shared store agents read/write without
direct addressing; coordination emerges from the shared state ("swarm agents
coordinate through shared state… without peer-to-peer connections"
([gurusup](https://gurusup.com/blog/agent-orchestration-patterns)),
[Claude Fleet]). Your `blackboard` kind is last-value-wins with non-destructive
reads — correct. The classic blackboard adds **knowledge sources that wake on
writes**: agent B is *notified* when the cell it cares about changes, rather than
polling. With the epoll loop (1.3) you can deliver a blackboard-changed nudge to
declared watchers, turning a passive store into a reactive coordination surface
without inventing a new topic kind.

---

## Category 3 — Agent Types, Pipelines & Observability

### 3.1 The big one: dynamic workflows are the *anti*-claude-bus — learn the boundary

**What it is.** Anthropic shipped **dynamic workflows** with Opus 4.8 on
2026-05-28. Claude writes a **JavaScript orchestration script** for your task;
a runtime executes it, fanning out **up to 16 concurrent / 1000 total**
subagents. Critically: *"The plan lives in script variables, not Claude's
context window. Intermediate results live in script variables instead"* — so the
orchestrator's context holds only the final answer. It has a built-in
**adversarial verify-and-converge loop**: *"Agents address the problem from
independent angles. Other agents then try to refute those findings. The run
iterates until the answers converge."* It's **resumable** (*"an interrupted job
resumes within the same session; completed agents return cached results"*) and
the script *"cannot touch the filesystem or shell. Only the agents read, write,
and run commands."*
([MarkTechPost](https://www.marktechpost.com/2026/05/28/anthropic-ships-claude-opus-4-8-alongside-dynamic-workflows-and-cheaper-fast-mode-with-workflows-capped-at-1000-subagents/),
[TechCrunch](https://techcrunch.com/2026/05/28/anthropic-releases-opus-4-8-with-new-dynamic-workflow-tool/)).

**The principle — context is the scarce resource.** The deep insight in dynamic
workflows is that *the plan and intermediate results don't belong in the LLM
context window*. A context window is finite, expensive, and degrades with length;
a script variable is infinite, free, and exact. By moving orchestration state
*out* of the prompt and into code, you scale fan-out arbitrarily while the
orchestrator stays lean. This is the same pressure behind "skill libraries" and
"external memory files" — keep the model's working set tiny, push everything else
to durable substrate.

**Why this is NOT what claude-bus should become — and where it IS relevant.**
Dynamic workflows are *ephemeral, headless, in-process, single-final-answer*.
claude-bus is *durable, pane-backed, human-attachable, ongoing*. These are
complementary, not competitive:

- A claude-bus **agent** is the right home for a long-lived role with a human who
  might grab the wheel. A dynamic workflow is the right tool for a *bounded
  fan-out subtask that agent wants to run* (e.g., "review these 40 files from 40
  angles and converge"). The agent *invokes* a dynamic workflow as a tool; it
  does not *become* one.
- **Steal the two patterns, not the architecture:**
  1. **Plan-in-code, not in-context.** Where claude-bus orchestrates
     (the `dispatch` skill, broadcast fan-out), keep the plan/cursor/results in
     broker state (you do!) rather than re-narrating them into an agent's
     context. You're already philosophically aligned; name it.
  2. **Adversarial verify-and-converge.** This is a *coordination pattern* you
     can run *over panes*: dispatch the same task to 2–3 agents, then a judge
     agent refutes/reconciles. See 3.3.

### 3.2 Long-running agents that learn over time (the durable-agent superpower)

**The principle.** claude-bus's durable agents have something dynamic workflows
structurally *cannot*: continuity across sessions. That's the substrate for
**learning over time**. The literature converges on a four-store model (CoALA):
*working* (the live context), *episodic* (what happened — interaction traces),
*semantic* (how things work — distilled facts), *procedural* (skills)
([CoALA](https://arxiv.org/pdf/2309.02427),
[memory survey](https://arxiv.org/html/2603.07670v1)). The learning *mechanism*
is **reflection**: a background pass reads episodic traces, extracts the
generalizable lesson, and writes it to semantic/procedural memory so future runs
condition on it (Reflexion stores reflective text on failures; Voyager writes
*successful solutions as reusable code skills indexed by natural-language
description* and composes them later
([Voyager/skill library](https://arxiv.org/html/2602.20867v1))).

The crucial nuance from 2026 practice: **self-improving skills don't touch the
model — they maintain an external `learnings.md` that's injected at runtime**,
updated incrementally after each run; expect *"five to ten run cycles before
clear behavioral changes"*
([MindStudio](https://www.mindstudio.ai/blog/self-improving-ai-skills-claude-code)).
This is exactly your existing `MEMORY.md` + auto-memory discipline, generalized.

**Map to claude-bus.**
- **Episodic = `events.jsonl` + transcripts.** You already have the trace. The
  missing piece is the *reflection pass*.
- Add a **reflection cron** (3.6): a scheduled agent reads the day's
  `events.jsonl` for a given agent, distills "what wedged / what worked," and
  appends to that agent's `learnings.md`, which `SessionStart` injects via
  `additionalContext`. This is a Voyager skill-library loop bolted onto your
  durable agents — a thing dynamic workflows can't do because they evaporate.
- **Procedural memory = skills that grow.** When an agent solves a recurring
  task, have it write a `.claude/skills/<name>/SKILL.md`. Over time the fleet
  accumulates a shared, version-controlled skill library — but *prune monthly*
  (the 2026 consensus: 8–12 skills, delete anything not triggered in 30 days
  ([developersdigest](https://www.developersdigest.tech/blog/best-claude-code-skills-2026))).

### 3.3 Pipeline & fan-out shapes — pick by DAG structure, not fashion

**The principle.** The orchestration topology should follow the *task's*
dependency structure. Recent work shows DAG properties — parallelism width,
critical-path depth, inter-subtask coupling — *predict the optimal topology*
([AdaptOrch](https://arxiv.org/pdf/2602.16873)). Practically, four shapes cover
nearly everything ([digitalapplied](https://www.digitalapplied.com/blog/multi-agent-orchestration-5-patterns-that-work)):

| Shape | When | claude-bus mapping |
|---|---|---|
| **Pipeline** (A→B→C) | strong sequential coupling | one agent's `inbox` feeds the next; cursor = stage gate |
| **Fan-out / scatter-gather** (map-reduce) | independent subtasks, wide | your `broadcast` + a gather topic (work-queue) |
| **Supervisor / worker tree** | dynamic decomposition | the `dispatch` skill + per-worker inbox; lead reconciles |
| **Adversarial verify-and-converge** | correctness-critical | dispatch same task to N panes → judge agent refutes/merges |

**The pattern worth adding: judge-panel / verify-and-converge over panes.** Dispatch
a high-stakes task to 2–3 agents independently, then a **judge agent** reads all
outputs and either reconciles or sends refutations back through the bus until they
agree. This is the dynamic-workflows convergence loop, realized with durable
panes — and the human can watch the disagreement play out live, which is *more*
debuggable than a headless workflow. It needs nothing new: `broadcast` to fan out,
an inbox for the judge, `mail` for refutations.

### 3.4 Triage / supervisor agent on escalation

**The principle.** Your DLQ escalates to `inbox-human`. But many escalations are
*mechanically fixable* (agent wedged → needs a nudge; context full → needs
`/compact`; boot stuck → needs restart). amux's **self-healing watchdog** does
exactly this autonomously: *"auto-compacts, restarts, and replays the last
message… context exhaustion triggers auto-compaction; thinking-block corruption
triggers restart with message replay"* ([amux](https://github.com/mixpeek/amux)).
The principle: **a supervisor that can take the same recovery actions a human
would, gated by confidence, reserving the human inbox for genuine novelty.**

**Map to claude-bus.** A **triage agent** (or broker logic) subscribes to the
`audit`/escalation topic. For known signatures it self-heals: STUCK → `bus msg
send` nudge; COMPACTING-needed → `slash /compact`; BOOT_STUCK → respawn the tab.
Only unrecognized failures reach `inbox-human`. Your memory already lists "triage
agent on escalation" as a surfaced idea — this is the concrete shape, with amux
as the proof it works unattended.

### 3.5 Observability: adopt OpenTelemetry GenAI semantic conventions

**The principle.** Ad-hoc dashboards don't compose; a *standard* does. The
OpenTelemetry **GenAI semantic conventions** (stabilizing through 2026) define a
common vocabulary: an `invoke_agent` span with child `chat` spans per LLM call
and `execute_tool` spans per tool, carrying `gen_ai.request.model`,
`gen_ai.usage.input_tokens`, `gen_ai.usage.output_tokens`
([opentelemetry GenAI](https://opentelemetry.io/blog/2026/genai-observability/),
[Zylos](https://zylos.ai/research/2026-02-28-opentelemetry-ai-agent-observability)).
**Claude Code has OTel instrumentation built in** — it "records spans around each
model request and tool execution, emits metrics for token and cost counters" and
exports OTLP to Honeycomb/Datadog/Grafana/Langfuse
([Claude Code observability](https://code.claude.com/docs/en/agent-sdk/observability)).
The killer diagnostic the spec enables: *"an agent that uses 50,000 tokens to
answer a question that normally takes 3,000 is misbehaving — without per-span
token accounting, this is invisible."*

**Map to claude-bus.** You compute CTX% by tailing transcript `usage` fields —
keep that for the live `bus monitor`, but *also* set Claude Code's OTel env vars
so each agent exports spans to a local collector (or Langfuse). Then
cross-agent traces, per-agent cost, and "which agent is burning tokens" become
queryable instead of eyeballed. This is the cross-agent view that scrollback
can't give you — exactly the second observability channel your CLAUDE.md already
names. Low effort (env vars + a local collector), high payoff (real cost/latency
attribution, replay).

### 3.6 Scheduled / cron agents

**The principle.** Not all agent work is interactive. Reflection (3.2), skill
audits, DLQ redrive, and health checks are *periodic background* work. The
2026 pattern is a scheduled headless `claude -p` invocation (cron, or Claude
Code's `/schedule` routines) that runs, acts, and exits — no pane needed because
no human watches it.

**Map to claude-bus.** A nightly cron that (a) runs the reflection pass per
agent, (b) audits the skill library for stale skills, (c) summarizes the day's
escalations into `inbox-human`. These are headless `claude -p` jobs — *here* the
SDK/headless path is unambiguously right, because there's no human surface to
preserve.

---

## Category 4 — Claude Code Ecosystem Craft

### 4.1 CLAUDE.md as evolving memory — keep it lean

**The principle.** A model reliably follows ~150–200 distinct instructions; the
system prompt eats ~50, so CLAUDE.md effectively gets 100–150 slots before Claude
*starts dropping instructions* — the practical ceiling is **80–120 high-signal
lines** ([Bijit Ghosh guide](https://medium.com/@bijit211987/the-complete-guide-to-claude-md-memory-rules-loading-and-cross-tool-compression-97cc12ed037b)).
Past that, more rules *reduce* compliance. This is why your MEMORY.md policy
("consolidate at 200 lines, promote stable knowledge to project config") is
correct — it's the same pressure. The project CLAUDE.md is long; some of it
(the full broker spec) might move to `docs/` and be `@`-referenced, reserving the
top-level file for rules Claude must hold *every turn*.

**Map to claude-bus.** Tier the memory: **CLAUDE.md** = inviolable rules
(jj-not-git, edit settings/ not .claude/, no-secrets); **`docs/`** = deep
reference (`@`-linked, loaded on demand); **per-agent `learnings.md`** = the
evolving episodic→semantic distillate (3.2). Customize compaction with a CLAUDE.md
line like *"when compacting, preserve modified-file list and the broker invariants"*
([skillsplayground](https://skillsplayground.com/guides/claude-code-memory/)).

### 4.2 High-value hooks — the full protocol you can exploit

**The principle.** Hooks are *deterministic* code at lifecycle points; unlike
prompts, they don't depend on the model's interpretation. The exact control
surface ([hooks reference](https://code.claude.com/docs/en/hooks)):

- **Exit 0** → stdout parsed as JSON for decisions. **Exit 2** → blocking error,
  stderr fed back to Claude. Other → non-blocking.
- **`UserPromptSubmit`**: `hookSpecificOutput.additionalContext` is *"wrapped in a
  system reminder and inserted into the conversation"* — Claude reads it next
  turn but it's not a chat message. `decision: "block"` + `suppressOriginalPrompt`
  can rewrite a prompt.
- **`PreToolUse`**: `hookSpecificOutput.permissionDecision` ∈ `allow|deny|ask|defer`.
- **`Stop`**: `decision: "block"` *prevents Claude from stopping* and continues.
- **`SessionStart`**: load context (inject `learnings.md` here).
- **`PreCompact`**: back up transcript / preserve invariants before compression.

**Map to claude-bus.** You already use `log-event.sh`. Two high-leverage adds:
1. **`SessionStart` → inject `learnings.md`** via `additionalContext`. This is
   the delivery mechanism for 3.2's learned memory.
2. **`UserPromptSubmit` → control-FIFO drain** (1.1): the hook reads any pending
   framed messages from `$STATE/ctrl/<agent>.fifo` and emits them as
   `additionalContext`, so the broker never writes the TTY. This is the cleanest
   path to the hybrid transport and uses *only* documented hook behavior.
3. **`Stop` ACK** you already derive — formalize it as the blocking-slash ACK in
   the reducer (1.2).

> **Skeptic's note.** Hooks run synchronously and block the agent; keep them
> fast (<100 ms) and never let a hook do network I/O or you'll wedge every turn.

### 4.3 Skills, subagents, slash commands — use the right tool

**The principle (the distinction that matters).**
- **Slash command** = a prompt template; inserts text. Cheap, no isolation.
- **Skill** = a named instruction bundle with progressive disclosure; loads only
  when invoked, can run `context: fork` in an isolated subagent, can preload
  tools. The right home for *reusable procedure*.
- **Subagent** (Task tool / `.claude/agents/*.md`) = a *separate context window*.
  The right tool when you need **context isolation or parallelism** — verbose
  searches, multi-angle review — without polluting the main thread.

([ofox guide](https://ofox.ai/blog/claude-code-hooks-subagents-skills-complete-guide-2026/)).

**Map to claude-bus.** Your `dispatch`/`draft`/`peek`/`status` are already skills
— correct. The subagent insight: an agent running a heavy investigation should
spawn a *subagent* (in-context, ephemeral) rather than `mail`-ing a peer pane,
*unless* the work needs the peer's durable state or human attachability. Rule of
thumb that fits your "pane = mailbox" memory: **bus = durable/attachable peers;
subagent = ephemeral context isolation; dynamic workflow = bounded mass fan-out.**

### 4.4 Settings & config discipline

Your single canonical `settings/claude-settings.json` symlinked fleet-wide is
*exactly* the 2026 best practice ("treat your skill folder like a dotfiles repo:
small, opinionated, version-controlled"). The one addition: **OTel env vars**
(3.5) belong here so every agent exports telemetry uniformly. Settings precedence
is enterprise > CLI > project-local > project > user; keep secrets out of the
tracked file (your CLAUDE.local.md rule already enforces this).

---

## Category 5 — Synthesis: If I Were Improving claude-bus Next, In Order

Ranked by **payoff ÷ effort**, tied to what already exists. The unifying theme:
*claude-bus's durability + human-attachability is the moat; spend effort
hardening the fragile transport and turning the durable substrate into learning,
not on chasing headless fan-out.*

### The top 5 highest-leverage moves

| # | Move | Why it's leverage | Effort | Payoff |
|---|---|---|---|---|
| **1** | **Hybrid transport: control-FIFO drained by the agent via `UserPromptSubmit` hook, not broker→TTY write** (1.1, 4.2) | Kills the single load-bearing fragility (TTY race, paste loss, keyboard contention) using *only* documented hook behavior, while *keeping* pane attachability. Structural, not incremental. | Med | **Very High** |
| **2** | **Event-sourced typed state reducer** replacing regex-over-events (1.2) | Turns "mid-stream dropped turn" and "broker wedge" from production mysteries into failing unit tests. Foundation everything else rests on. | Med | **High** |
| **3** | **Reflection cron → per-agent `learnings.md` injected at `SessionStart`** (3.2, 3.6, 4.2) | Activates the durable-agent superpower dynamic workflows *can't* have: learning over time. Low risk, compounding return. | Low–Med | **High** |
| **4** | **OpenTelemetry GenAI export** (env vars + local collector/Langfuse) (3.5) | Built into Claude Code; flip env vars and get cross-agent traces, per-agent cost, token-anomaly detection — the cross-agent view scrollback can't give. | **Low** | **High** |
| **5** | **Triage/self-healing supervisor on escalation** (3.4) | amux-proven. Reserves `inbox-human` for genuine novelty; auto-nudges STUCK, auto-`/compact`, auto-respawn BOOT_STUCK. | Med | High |

### The rest, ranked

6. **Idempotency-key dedup on the agent drain** (2.1) — closes at-least-once
   double-injection. Low effort, real correctness. *(Pairs with #1.)*
7. **`bus msg redrive`** to make the DLQ re-drivable (2.2) — turns escalation
   from dead end into loop. Trivial effort.
8. **epoll + inotify + timerfd event loop** replacing the 250 ms poll (1.3) —
   sub-ms latency, ~0 idle CPU, no loop body to wedge. Do *after* #1/#2.
9. **Judge-panel / verify-and-converge over panes** (3.3) — the dynamic-workflows
   convergence pattern, realized durably and human-watchable. Reuses
   broadcast+inbox; pure orchestration.
10. **Skills-that-grow with monthly pruning** (3.2, 4.3) — procedural memory for
    the fleet. Compounds slowly; prune aggressively.
11. **SQLite-WAL for mutable coordination state** (claims/cursors/in-flight) (2.4)
    — crash-safe, atomic CAS for work-queue. *Only if* consumers ever bypass the
    broker; keep append-log for the immutable event stream.
12. **CLAUDE.md tiering** (4.1) — move the broker spec to `@`-linked `docs/`,
    keep the top file under ~120 high-signal lines.

### What to deliberately NOT do

- **Don't rewrite on the Agent SDK / become dynamic-workflows.** That trades away
  human-attachability — the entire reason this isn't `amux`. Use headless
  `claude -p` for *background/cron* jobs and *invoke* dynamic workflows as a
  bounded fan-out *tool*; don't make your durable agents headless.
- **Don't use `TIOCSTI`.** Dead since Linux 6.2; security hole.
- **Don't SQLite-ify the topic logs.** Append-only files are already crash-safe,
  replayable, and grep-able — that's what an audit bus wants.
- **Don't reach for io_uring.** epoll is the right level for a single-host broker;
  io_uring is complexity you won't recover.
- **Don't grow CLAUDE.md.** More rules past ~150 *lowers* compliance.

---

## Sources

**Anthropic primary surfaces**
- [Dynamic workflows & Opus 4.8 — MarkTechPost](https://www.marktechpost.com/2026/05/28/anthropic-ships-claude-opus-4-8-alongside-dynamic-workflows-and-cheaper-fast-mode-with-workflows-capped-at-1000-subagents/)
- [Opus 4.8 dynamic workflow tool — TechCrunch](https://techcrunch.com/2026/05/28/anthropic-releases-opus-4-8-with-new-dynamic-workflow-tool/)
- [Claude Code hooks reference](https://code.claude.com/docs/en/hooks)
- [Hooks/subagents/skills complete guide — ofox.ai](https://ofox.ai/blog/claude-code-hooks-subagents-skills-complete-guide-2026/)
- [Claude Code best practices](https://code.claude.com/docs/en/best-practices)
- [Agent SDK — MCP transports](https://code.claude.com/docs/en/agent-sdk/mcp)
- [Agent SDK — streaming output](https://code.claude.com/docs/en/agent-sdk/streaming-output)
- [Claude Code OpenTelemetry observability](https://code.claude.com/docs/en/agent-sdk/observability)
- [`--input-format stream-json` (undocumented) — claude-code#24594](https://github.com/anthropics/claude-code/issues/24594)
- [Inside the Agent SDK: stdin/stdout — buildwithaws](https://buildwithaws.substack.com/p/inside-the-claude-agent-sdk-from)

**Text injection / PTY mechanics**
- [pty(7) man page](https://www.man7.org/linux/man-pages/man7/pty.7.html)
- [Bracketed-paste loss after reattach — pi#2704](https://github.com/earendil-works/pi/issues/2704)
- [Multi-line paste line-splitting — openclaw#18809](https://github.com/openclaw/openclaw/issues/18809)
- [tmux send-keys / scripting](https://tao-of-tmux.readthedocs.io/en/latest/manuscript/10-scripting.html)

**Transports & resiliency**
- [Event sourcing with SQLite](https://www.sqliteforum.com/p/event-sourcing-with-sqlite)
- [SQL event store with dedup & ordering — mattbishop](https://github.com/mattbishop/sql-event-store)
- [Dual-contract event store in SQLite](https://medium.com/@impactarchitecture/persistence-model-for-a-dual-contract-event-store-in-sqlite-53f3505f7d21)
- [amux — agent multiplexer (watchdog, SQLite CAS task claiming)](https://github.com/mixpeek/amux)

**Memory & learning**
- [CoALA — Cognitive Architectures for Language Agents](https://arxiv.org/pdf/2309.02427)
- [Memory for Autonomous LLM Agents — survey](https://arxiv.org/html/2603.07670v1)
- [SoK: Agentic Skills (Voyager skill libraries)](https://arxiv.org/html/2602.20867v1)
- [Self-improving Claude Code skills — MindStudio](https://www.mindstudio.ai/blog/self-improving-ai-skills-claude-code)
- [Best Claude Code skills 2026 — Developers Digest](https://www.developersdigest.tech/blog/best-claude-code-skills-2026)

**Orchestration patterns**
- [5 orchestration patterns that work — Digital Applied](https://www.digitalapplied.com/blog/multi-agent-orchestration-5-patterns-that-work)
- [Swarm vs mesh vs hierarchical — gurusup](https://gurusup.com/blog/agent-orchestration-patterns)
- [AdaptOrch: task-adaptive orchestration (DAG topology)](https://arxiv.org/pdf/2602.16873)

**Observability**
- [OpenTelemetry GenAI observability (2026)](https://opentelemetry.io/blog/2026/genai-observability/)
- [OTel for AI agents — Zylos](https://zylos.ai/research/2026-02-28-opentelemetry-ai-agent-observability)
- [OTel for LLM observability — Langfuse](https://langfuse.com/integrations/native/opentelemetry)

**C++ event-loop craft**
- [Notes on epoll and io_uring](https://iafisher.com/notes/2025/10/epoll-io-uring)
- [A universal I/O abstraction for C++ (io_uring) — cor3ntin](https://cor3ntin.github.io/posts/iouring/)

**Claude Code config / memory craft**
- [Complete guide to CLAUDE.md — Bijit Ghosh](https://medium.com/@bijit211987/the-complete-guide-to-claude-md-memory-rules-loading-and-cross-tool-compression-97cc12ed037b)
- [Claude Code memory guide 2026 — skillsplayground](https://skillsplayground.com/guides/claude-code-memory/)
