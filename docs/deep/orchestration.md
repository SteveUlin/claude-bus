# Orchestration, Pipelines & Novel Agent Types — A Deep Reference for claude-bus

> **FROZEN — pre-refactor archaeology (Phase-4 doc cleanup, 2026-06-03).**
> Documents the pre-shatter `delivery::Loop` architecture. Not maintained
> through the broker-seam refactor (`docs/broker-seam-redesign.md`); a lean set
> mapped to Log / Router / Transport / Readers is regenerated after Phase 2
> lands. Historical context, not current truth.

> Written 2026-05-28. The deep companion to `docs/modern-agent-techniques.md`,
> narrowed to one question: **how should a durable, pane-backed agent fleet
> compose, schedule, supervise, and self-heal work?** Every section leads with
> the *principle* — the mechanism, the tradeoff, the design pressure — then the
> concrete mechanics (with quoted source from the implementations that get it
> right), then maps to claude-bus. Skim the tables; read the synthesis (§9) for
> the ordered backlog.

The prior survey's thesis still holds and frames everything below: **claude-bus
occupies the durable/pane-backed/human-attachable corner; the industry sprints
toward ephemeral/headless/cloud fan-out (Anthropic's dynamic workflows, shipped
the day this was written, is the purest example). The right move is not to chase
that corner but to (a) steal the corner-agnostic resiliency and orchestration
patterns and (b) sharpen what claude-bus is uniquely good at.** This doc is the
orchestration-specific version of that argument.

One sentence to anchor the whole document: **claude-bus already has a
coordination substrate that most "orchestrators" lack — a single broker that
owns typed topics, cursors, in-flight tracking, and retry/escalate — but it has
no *scheduler*, no *supervisor*, and no *composition primitive above a single
mail*. Those three absences are the whole opportunity.**

---

## 1. The core principle: orchestration topology should follow the task's dependency structure, not fashion

**The principle.** The single most important idea in multi-agent orchestration
is that the *shape* of your agent graph should be derived from the *shape* of the
task's dependency DAG — its parallelism width (how many independent subtasks),
its critical-path depth (longest chain of dependencies), and its inter-subtask
coupling (how much one subtask's output constrains another). A task that is
embarrassingly parallel wants fan-out/scatter-gather; a task that is a strict
chain wants a pipeline; a task that is correctness-critical and under-specified
wants adversarial verify-and-converge. Picking the wrong topology is the most
common and most expensive orchestration mistake: you serialize what could
parallelize (slow), or you parallelize what has hidden coupling (incoherent
merge). The 2026 literature now treats topology selection as a first-class,
*task-adaptive* decision rather than a fixed architecture choice.

**The four shapes that cover nearly everything**, and how each maps to the
broker primitives claude-bus already has:

| Shape | Dependency signature | Mechanism | claude-bus mapping (today / gap) |
|---|---|---|---|
| **Pipeline** A→B→C | strong sequential coupling, depth ≫ width | each stage's output feeds the next stage's input | one agent's `inbox-B` is fed by A's completion; **gap: nothing makes "A done" trigger "mail B"** |
| **Fan-out / scatter-gather** (map-reduce) | wide, independent subtasks; one reducer | producer sprays N work items; N workers pull; a gather step reduces | `broadcast` fans out; `work-queue` is the pull side; **gap: no gather/join barrier** |
| **Supervisor / worker tree** | dynamic decomposition; lead reconciles | a coordinator decomposes, dispatches, collects, re-dispatches | auri is the coordinator; `bus msg mail` is dispatch; **gap: no structured result collection, no claim atomicity** |
| **Adversarial verify-and-converge** | correctness-critical, under-specified | dispatch same task N ways → judge refutes → iterate to convergence | `broadcast` + a judge inbox + `mail` refutations; **gap: no convergence loop, no judge role** |

**Why this matters more for claude-bus than for a headless orchestrator.** A
headless fan-out (dynamic workflows) picks its topology *inside a script the
human never sees*. claude-bus runs every branch in a *pane the human can watch
and grab*. That means the topology isn't just a performance choice — it's a
**legibility** choice. A verify-and-converge run over three panes is something
sulin can literally watch disagree and resolve; the same run inside a JS
orchestration script is an opaque box that emits one answer. The durable-pane
substrate makes the *more debuggable* topologies (judge panels, supervisor trees)
strictly more valuable than they are anywhere else.

*Sources:* AdaptOrch task-adaptive topology ([arXiv 2602.16873](https://arxiv.org/pdf/2602.16873));
5 orchestration patterns ([Digital Applied](https://www.digitalapplied.com/blog/multi-agent-orchestration-5-patterns-that-work));
swarm vs hierarchical ([gurusup](https://gurusup.com/blog/agent-orchestration-patterns)).

---

## 2. Fan-out / scatter-gather and the join barrier (the missing primitive)

**The principle.** Fan-out is trivial; *gather* is the hard part. Spraying N
independent subtasks to N workers is one `for` loop. The difficulty is the
**join barrier**: knowing when all N (or a quorum of N) have completed, collecting
their results in one place, and handing the reduced result to a downstream step —
*without* the orchestrator having to poll, hold all N results in its context
window, or block on the slowest worker forever. Every scatter-gather system lives
or dies on how it implements the barrier and the timeout/partial-result policy.

**How dynamic workflows do it (the reference, even if you don't adopt it).**
Anthropic's dynamic workflows put the join barrier *in a script variable, not in
the model's context*. From the announcement coverage: *"The plan moves into code,
not Claude's context window. Intermediate results live in script variables
instead, so Claude's context holds only the final answer."* The runtime *"fans
work across subagents running in parallel,"* allows *"up to 16 concurrent agents
and caps each run at 1,000 agents total,"* and is resumable: *"progress is saved
as the run proceeds… completed agents return cached results on resume."* The
deep insight is the **context/coordination-state split**: the orchestration state
(which subtasks dispatched, which completed, their results) is infinite, free,
and exact when it lives in a variable; it is finite, expensive, and lossy when it
lives in a prompt. *This is the principle to steal even though the architecture
isn't.*

**How amux does it (the durable-state reference).** amux has no in-process
script; its coordination state lives in **SQLite**. Tasks are `issues` rows with
a `status` lifecycle (`todo → doing → done/discarded`). A worker session "joins"
by transitioning rows; the board *is* the barrier — you query for `status='done'`
to know what finished. The gather is a SQL `SELECT`, durable across restarts. The
tradeoff vs. dynamic workflows: amux's state survives a crash (it's on disk) but
it's a passive store you poll, not an active promise you await.

**The claude-bus gap and the principled fix.** claude-bus has the *scatter*
(`broadcast` fans to N inboxes, `work-queue` is the pull side) but **no gather
barrier**. There is no topic kind that means "collect N results and signal when
complete." The principled fix follows the context/coordination split directly:

- Add a **gather/join topic kind** (or a convention on `work-queue`): the
  coordinator enqueues N items each stamped with a shared `correlation` id (the
  wire format *already carries a 16-byte `correlation` field* — `topic_log.h`
  lines 59, 69 — and it is currently unused for this). Workers write results
  back to a `results-<correlation>` topic. The broker tracks "N expected, M
  received" in an in-flight-style tracker and, on M==N (or quorum, or timeout),
  emits one `gather-complete` record to the coordinator's inbox carrying the
  *pointer* to the collected results, not the results themselves.
- The coordinator's context never holds the N intermediate results — it reads
  them on demand via `bus msg body`. This is *exactly* the dynamic-workflows
  "results live in variables, context holds only the final answer" pattern,
  realized over durable topics instead of script variables.

| Gather mechanism | State location | Survives crash? | Polls or awaits? | Fits claude-bus |
|---|---|---|---|---|
| Dynamic-workflows script var | in-process JS | no (resumable within session) | awaits (promises) | the *pattern* to steal |
| amux SQLite `status` board | on-disk DB | yes | polls (`SELECT`) | durable, but passive |
| **claude-bus correlation + results topic + broker barrier** | on-disk topic log + broker memory | yes | broker pushes on completion | **recommended** — reuses the unused `correlation` field |

---

## 3. Atomic task claiming (CAS) — do you actually have the race?

**The principle.** When multiple workers pull from one queue, the canonical bug
is **double-claim**: two workers read the same head item and both start it. The
textbook fix is compare-and-swap: a worker atomically transitions the item from
`unclaimed` to `claimed-by-me` and checks that exactly one row changed. The
database (or the single serializing process) is the arbiter; no item is ever
worked twice.

**How amux does it — and the subtlety.** amux claims via SQLite UPDATEs:
`UPDATE issues SET status='doing', updated=? WHERE id=?` (amux-server.py ~line
4120, in `_pickup_next_board_task`). Note this is *not* a guarded CAS in the
multi-worker sense — amux relies on SQLite's single-writer serialization plus the
fact that one session pulls its own next task. The honest version of CAS is
`UPDATE ... SET owner=? WHERE id=? AND owner IS NULL` then check `rows_affected ==
1`; amux's notification path (`_notify_session_of_task`, marking `notified=1`
idempotently per `(session, item_id)`) shows it *does* lean on row-level
idempotency to avoid double-acting.

**The claude-bus reality — you probably don't have the race.** This is the
skeptic's note that matters: **claude-bus's `work-queue` fetch is serialized
through the broker.** `bus msg fetch` is an RPC to a single-threaded broker; the
fetch handler reads the cursor, peeks one record, and advances the cursor
(`broker.cpp` lines 609–658), all on one thread with no concurrency. Two
consumers calling `fetch` simultaneously are serialized by the RPC accept loop —
the cursor moves once per fetch, so each consumer gets a *distinct* record by
construction. **You do not need SQLite CAS unless a consumer ever bypasses the
broker and reads the topic log directly.** The append-log + single-broker design
already gives you atomic single-assignment for free.

Where the latent risk *is*: the cursor is a single `_default` cursor per topic
(`topic_log.h` `cursorPath`, consumer `""` → `_default`). The docs say
work-queue is "multi-consumer pull… each fetch advances the cursor" and
"multiple consumers each get distinct records" — and they do, but they all share
one cursor, so it's really a *shared work-stealing queue*, not per-consumer
streams. That's correct for a work queue (you *want* distinct records) but it
means **there is no way to replay a work-queue per-consumer** and **no record of
who claimed what**. If you ever want "worker X is doing item 7" visibility (you
will, for the supervisor in §6), add an owner stamp at fetch time.

*Sources:* amux SQLite claiming (`amux-server.py` `_pickup_next_board_task`,
`_notify_session_of_task`); contract-net / auction allocation background
([Nature Sci Rep 2025](https://www.nature.com/articles/s41598-025-21709-9),
[auction-based agent interaction](https://arxiv.org/html/2511.13193v1)).

---

## 4. Pipelines: the trigger is the whole problem

**The principle.** A pipeline A→B→C is only as good as its *stage gates*. The
naive version ("A mails B when done") puts the orchestration logic in A's prompt —
fragile, because A must remember to do it, must know B's name, and must format the
handoff. The robust version makes the *substrate* enforce the gate: B's input
topic is fed automatically when A's output condition is met, and the cursor on
B's input *is* the stage gate (B can't start until the record exists).

**The completion-detection problem, and amux's `done_pattern`.** The hard part of
a substrate-enforced pipeline is detecting "A is done." amux solves this with a
**completion FSM driven by regex over terminal output**: a schedule can carry a
`done_pattern` (regex) and a `done_action` ∈ `{disable, notify, command:<follow-up>}`
(`_watch_schedule_response`, amux-server.py ~4547). It polls `tmux_capture` every
5s, matches the new output against the pattern, and on match fires the action —
including *chaining the next stage* via `command:<follow-up>`. This is a real,
working, regex-triggered pipeline primitive. Its weakness is exactly the weakness
the prior survey flagged for claude-bus's own state inference: **regex over
terminal scrollback is brittle by construction** (it breaks on format drift, can
match its own echo, needs the `pre_output` overlap-stripping hack to avoid
re-matching old text).

**The claude-bus fix — trigger on typed events, not scrollback.** claude-bus has
something amux doesn't: `events.jsonl` carries *typed* `Stop`/`UserPromptSubmit`/
`SessionEnd` events. A pipeline gate should fire on a *typed completion fact*, not
a regex match. Concretely: a `pipeline` topic kind (or a `deliver_after` field)
where a record for B is dispatched only after the broker observes A's `Stop`
event for a specific correlation id. The broker already folds `events.jsonl` in
`scanEvents` (`delivery.cpp` 231–399) and already uses `Stop` as the blocking-op
ACK — extend that same fold to release a gated pipeline record. **This is the
amux `done_pattern` pattern, but driven by typed events instead of regex, which
is strictly more robust.**

| Stage-gate trigger | Robustness | Where claude-bus stands |
|---|---|---|
| A's prompt mails B | brittle (relies on the model) | what happens today by convention |
| Regex over scrollback (amux `done_pattern`) | brittle (format drift, self-match) | amux's working primitive |
| **Typed `Stop`/completion event in the broker** | robust (structured fact) | broker already folds these — extend it |

*Sources:* amux `_watch_schedule_response` / `done_pattern` / `done_action`
(amux-server.py ~4547); claude-bus `Loop::scanEvents` (`delivery.cpp` 231–399).

---

## 5. Adversarial verify-and-converge & judge panels (the highest-value novel topology)

**The principle.** For correctness-critical, under-specified work, a single agent
is a single point of failure: it can be confidently wrong. The fix is
**ensemble + adversarial refutation + convergence**: dispatch the same task to N
agents *from independent angles*, then have other agents *try to refute* each
finding, then *iterate until the answers converge*. This is the core loop
Anthropic built into dynamic workflows — verbatim: *"Agents address the problem
from independent angles. Other agents then try to refute those findings. The run
iterates until the answers converge."* The principle is older than LLMs (it's
the Contract-Net / peer-review / Byzantine-agreement family), but the LLM twist
is that **disagreement between independent generations is a cheap, powerful error
signal** — if three independent agents converge, confidence is high; if they
diverge, you've found exactly the uncertain spot to escalate.

**Why this is the single best topology to add to claude-bus, and why it needs
almost no new code.** claude-bus's durable panes make this *more* valuable than
the headless version: the human watches the disagreement resolve live. The
mechanics reuse existing primitives entirely:

1. **Fan out** the same task to 2–3 coder panes via `broadcast` (sender-side
   fan-out to each agent's inbox — already implemented).
2. **Judge role** (new, §7): a long-lived `judge` agent whose inbox collects the
   N outputs (via the gather barrier from §2). It reads all N, identifies
   agreement/disagreement, and either accepts (converged) or mails *refutations*
   back to the dissenting agents through the bus.
3. **Converge** by iterating: each refutation is a fresh `mail`; the loop ends
   when the judge sees agreement or hits a round cap.

The only genuinely new pieces are the **judge role prompt** and the **round
counter** (which lives in broker state or the judge's own tracking, *not* in the
coordinator's context). Everything else — fan-out, inboxes, refutation mails — is
the existing bus.

**The ruflo angle, read skeptically.** ruflo markets "Queen-led hierarchy (Raft,
Byzantine, Gossip)" consensus topologies and a federation **trust-scoring formula**
(`0.4×success + 0.2×uptime + 0.2×threat + 0.2×integrity`). The consensus
vocabulary is mostly aspirational framing over an MCP-plugin CLI (the "neural /
self-learning SONA / ReasoningBank" claims have no visible implementation —
treat as vapor). But the *trust-weighting* idea has a kernel worth keeping: when
a judge reconciles N agents, **weight their votes by demonstrated reliability**
(an agent whose past outputs survived refutation gets a heavier vote). That's a
cheap, concrete refinement of a judge panel, and it's the one ruflo idea that
isn't hand-waving.

*Sources:* dynamic-workflows convergence loop ([MarkTechPost](https://www.marktechpost.com/2026/05/28/anthropic-ships-claude-opus-4-8-alongside-dynamic-workflows-and-cheaper-fast-mode-with-workflows-capped-at-1000-subagents/),
[TechCrunch](https://techcrunch.com/2026/05/28/anthropic-releases-opus-4-8-with-new-dynamic-workflow-tool/));
contract-net / auction negotiation ([apxml negotiation & consensus](https://apxml.com/courses/multi-agent-llm-systems-design-implementation/chapter-3-agent-communication-coordination/agent-negotiation-consensus));
ruflo topology/trust claims ([ruvnet/ruflo](https://github.com/ruvnet/ruflo) — read skeptically).

---

## 6. Supervisor / worker trees: OTP semantics applied to a pane fleet

**The principle.** The most battle-tested supervision model in computing is
Erlang/OTP's: a **supervisor** exists only to *start, monitor, and restart* its
children, and crucially, **a supervisor does no work itself** — it is pure
lifecycle management. The genius is the formalization of *restart policy*, which
turns "what do we do when a thing dies?" from ad-hoc into a small, total
decision table:

- **Restart strategies** — `one_for_one` (restart only the dead child),
  `one_for_all` (one dies → kill and restart all siblings), `rest_for_one`
  (restart the dead child and everything started after it), `simple_one_for_one`
  (dynamic pool of identical children added on demand).
- **Restart intensity (MaxR/MaxT)** — the critical anti-thrash mechanism: *"If
  more than MaxR number of restarts occur in the last MaxT seconds, the
  supervisor terminates all the child processes and then itself."* A crash loop
  is detected and *escalated upward* rather than retried forever.
- **Child restart type** — `permanent` (always restart), `transient` (restart
  only on abnormal exit), `temporary` (never restart).
- **Children start in order, terminate in reverse order** — startup/shutdown
  dependencies are encoded in list order.

**Why this is the right lens for claude-bus's fleet.** Map directly:

| OTP concept | claude-bus analog |
|---|---|
| Supervisor (does no work) | a `supervisor` role / broker logic that owns agent lifecycle, *not* a coder |
| Child process | an agent pane (`bus spawn NAME`) |
| `permanent` child | comms / auri / the standing fleet — always respawn |
| `transient` child | a worker spawned for one task — respawn only if it died mid-task |
| `temporary` child | a one-shot `claude -p` cron job — never respawn |
| `one_for_one` | respawn just the dead agent (the common case) |
| `rest_for_one` | a pipeline: if stage B dies, restart B and C but not A |
| **MaxR/MaxT intensity** | **the anti-thrash gate claude-bus is missing** (see flaw §8) |
| Escalate when intensity exceeded | mail `inbox-human` only when restart-looping, not on every failure |

**The MaxR/MaxT insight is the one to internalize.** claude-bus's broker retries
delivery 3× then escalates (correct), and amux's watchdog uses a `CLAUDE_COOLDOWN
= 900` window and an `UNHEALTHY_THRESHOLD = 3` (correct). But neither has OTP's
*"too many restarts in a window → stop trying and escalate the whole subtree."*
That's the principle that prevents a wedged agent from being respawned into the
same wedge forever, burning tokens. A supervisor should track restart timestamps
per agent and, on MaxR-in-MaxT, **stop auto-healing that agent and surface it to
the human as genuinely broken** — reserving human attention for the failures that
automation can't fix.

*Sources:* OTP supervisor principles ([erlang.org sup_princ](https://www.erlang.org/doc/system/sup_princ.html)).

---

## 7. The self-healing triage supervisor — amux is the working proof, with the full taxonomy

**The principle.** Most escalations are *mechanically fixable* by the same
actions a human would take: a wedged agent needs a nudge; a context-full agent
needs `/compact`; a crashed session needs a restart-and-replay. A triage
supervisor takes those actions *autonomously, gated by confidence and a cooldown*,
and reserves the human inbox for genuine novelty. The design pressure: human
attention is the scarcest resource in the whole system; spending it on "agent X
needs a nudge" is waste.

**amux's triage taxonomy — read this as a menu of recovery rules.** amux's
monitor loop (`amux-server.py` ~2122 onward) and standalone `watchdog.py` together
implement a remarkably complete recovery taxonomy. Each rule has a *signature*
(how it's detected), an *action*, and an *anti-thrash guard*:

| Signature (detected by) | Recovery action | Anti-thrash guard |
|---|---|---|
| Context low — regex `context left until auto-compact: N%`, `pct < 50` | back up JSONL, `send_text("/compact")`, set `post_compact_continue` | `now - last_compact > 300s` |
| Image dimension / corrupt-image error in scrollback | `/compact` to evict the bad image | per-error flag + `> 120s` |
| Thinking-block corruption (`redacted_thinking` + `cannot be modified`) | **hard-kill + restart + replay last meaningful user message** | `now - last_restart > 120s` |
| Session-ID-in-use conflict | hard-kill + restart + replay | `> 120s` |
| Claude exited to bare shell prompt | restart session (opt-in `CC_AUTO_CONTINUE`) | `> 90s`, plus process-level check for OOM kills |
| Stuck "waiting for user input" 2+ snapshots (~60s) | auto-respond to unblock | `_AUTO_RESPONSE_COOLDOWN` |
| Session > 48h old | recycle (hard-kill + restart) | `> 300s` since last restart |
| Idle > 30 min | **auto-hibernate** (stop Claude to reclaim 400–750 MB RSS); wakes on next send | startup grace period |
| Health endpoint dead 3× consecutive | restart server; **if still dead, spawn `claude -p` to diagnose + fix + commit + push** | `CLAUDE_COOLDOWN = 900s` |

The escalation ladder is the lesson: **restart → if restart fails, spawn an
agent to *fix the code*.** From `watchdog.py`'s `invoke_claude`: it builds a
diagnostics bundle (last 100 server-log lines, `ps` output, recent errors,
`/proc/meminfo`) and runs `claude --print --dangerously-skip-permissions -p
<prompt>` asking it to find the root cause, fix `amux-server.py`, verify with an
`ast.parse`, and `git push`. The server auto-restarts on file save. **This is a
self-modifying supervisor** — the deepest rung of self-healing.

**How this maps to claude-bus — you have the pieces, not the policy.**
claude-bus already detects the states (`agent_status.h` computes
Idle/Working/Stuck/Compacting/NeedsInput/BootStuck) and already has *one*
autonomous recovery rule: `maybeAutoClear` (`delivery.cpp` 805–905) enqueues
`/clear` to idle workers, with a careful gate list (last_event==Stop, idle ≥
threshold, inbox empty, no in-flight, not in blocking-op, pane alive, not a
high-continuity role, cooldown elapsed) and an audit trail. **That is exactly one
rule of amux's taxonomy, built correctly.** The opportunity is to generalize
`maybeAutoClear` into a **triage table** with the same shape — `(signature,
action, guard)` — covering the other recovery rules:

- **STUCK** (mid-stream dropped turn — your memory's known failure) → raw `bus
  msg send` nudge. *Signature already detectable; this is the one you currently
  fix by hand.*
- **Context high** (the token watcher in `maybeScanTokens` already computes
  CTX% — `delivery.cpp` 920–1015) → `/compact` when over a threshold, mirroring
  amux's proactive auto-compact. *You already have the numerator and denominator
  computed; the action is one enqueue.*
- **BOOT_STUCK** → respawn the tab.
- **Restart-loop** (MaxR/MaxT from §6) → stop healing, mail human.

Only unrecognized failures reach `inbox-human`/`inbox-ops`. Your memory already
lists "triage agent on escalation" as a surfaced idea — this section is the
concrete shape, with amux as the proof it runs unattended for weeks.

*Sources:* amux monitor recovery rules + `watchdog.py` (quoted inline above).

---

## 8. Novel & specialist agent types — beyond the coder fleet

**The principle.** claude-bus's roster today is one coordinator (auri), one human
amplifier (comms), and three *territory-partitioned coders* (bast = plumbing,
elodin = broker, kvothe = viewers). That's a clean **specialist-by-codebase-region**
decomposition — each agent owns a directory tree, routing is by ownership
(`roles/*.md`), and seams are surfaced before splitting. It's a good static
topology. But the durable-agent substrate enables *agent types that aren't coders
at all* — long-lived roles whose value is precisely that they persist, observe,
and accumulate across sessions. These are the agent types worth considering, each
justified by a principle:

| Agent type | Principle (why it exists) | What it does | Substrate it needs |
|---|---|---|---|
| **Supervisor** (§6/§7) | lifecycle ≠ work; pure monitoring is its own job | watches `events.jsonl` + `audit`, runs the triage table, respawns, escalates restart-loops | the triage table; restart-timestamp tracking |
| **Judge / critic** (§5) | independent refutation is a cheap error signal | collects N fan-out outputs, refutes, drives convergence; weights votes by reliability | gather barrier (§2); a round counter |
| **Librarian** | a fleet's knowledge decays without a curator | owns the shared skill library + `learnings.md`; prunes stale skills monthly (the 8–12 rule); reconciles cross-agent conventions | a writable docs/skills tree; a reflection cron |
| **Watcher** (passive) | some signals need continuous observation, not a turn | tails a blackboard/log cell and *nudges* declared subscribers on change (reactive blackboard) | a blackboard-changed trigger (broker push on write) |
| **Reflector** (cron, headless) | learning over time is the durable-agent superpower | nightly `claude -p` reads the day's `events.jsonl` per agent, distills lessons → `learnings.md` injected at `SessionStart` | headless `claude -p` cron; a `SessionStart` hook |
| **Triage spoke** | escalations need a first responder before the human | reads `inbox-ops`, classifies (mechanical vs novel), self-heals or forwards to `inbox-human` | subscription to `inbox-ops` |

**The key composition principle — durable agents *invoke* ephemeral fan-out;
they do not *become* it.** This is the boundary the prior survey drew, restated
for agent types: a claude-bus agent is the right home for a long-lived role with
a human who might grab the wheel. A dynamic workflow (or a Task-tool subagent) is
the right tool for a *bounded fan-out subtask that agent wants to run*. So:

- **Bus peer** = durable, attachable, persists across sessions (the standing
  fleet + the new specialist roles above).
- **Task-tool subagent** = ephemeral context isolation within one agent's turn
  (a heavy search, a multi-file review) — *not* a bus citizen (this matches your
  "pane = mailbox" memory exactly).
- **Dynamic workflow** = bounded mass fan-out (review 40 files from 40 angles and
  converge) — *invoked as a tool* by a durable agent, not a replacement for one.

The discriminator: **does the work need durable state or human-attachability?**
Yes → bus peer. No, but needs context isolation → subagent. No, and it's mass
parallel → dynamic workflow.

**A note on the static-vs-dynamic roster.** The territory-partitioned coder fleet
is a *static supervisor tree* — fixed children, fixed ownership. amux and dynamic
workflows both lean *dynamic* — workers spawned on demand for a task, then
discarded (`simple_one_for_one` in OTP terms). claude-bus can have both: keep the
standing specialists as `permanent` children, and let auri (or a supervisor)
spawn `transient`/`temporary` workers for bounded tasks via `bus spawn`. The
roster doesn't have to choose; the restart-type taxonomy from §6 lets the same
fleet hold both.

*Sources:* CoALA four-store memory model & reflection ([arXiv 2309.02427](https://arxiv.org/pdf/2309.02427));
skill-library curation / 8–12 rule (prior survey §3.2); OTP child types ([erlang.org](https://www.erlang.org/doc/system/sup_princ.html)).

---

## 9. How this maps to claude-bus — adopt / change / flaws spotted

This is the one section that's prescriptive. Everything above is the *why*; here's
the *what*, principle-first, with rough effort/payoff and file refs.

### 9.1 Flaws spotted in the current orchestration code

These are concrete, file-referenced issues found reading the source — surfaced
here, not as the doc's thesis.

1. **No join/gather barrier; the `correlation` field is dead.** The v4 wire
   format reserves a 16-byte `correlation` (RPC pairing) field
   (`topic_log.h` lines 59, 69; `Message::correlation`), but nothing in
   `broker.cpp`/`delivery.cpp` reads or writes it for scatter-gather. Fan-out
   exists (`broadcast`); the matching gather does not. *This is the single
   biggest orchestration gap — there is no way to express "collect N results."*

2. **`maybeAutoClear` is a one-off, not a triage table.** The auto-clear rule
   (`delivery.cpp` 805–905) is well-built but hard-codes a single
   `(signature=idle, action=/clear)` rule with its own ad-hoc cooldown map
   (`auto_clear_next_allowed_ms_`) and skip-roles set. Adding the STUCK-nudge or
   high-CTX-compact rules means copy-pasting this whole structure. It wants to be
   a table of `(signature, action, guard)` rows so new recovery rules are data,
   not duplicated control flow. (amux's monitor is the same shape done as a long
   if-ladder — claude-bus can do better with a table.)

3. **No restart-intensity (MaxR/MaxT) anti-thrash for agents.** Delivery retries
   are capped at `kMaxAttempts` (good), and auto-clear has a 5-min cooldown
   (good), but there is no fleet-level "this agent has been auto-recovered N
   times in M minutes → stop and escalate." Without it, a generalized triage
   table (flaw #2's fix) could respawn/nudge a genuinely broken agent forever.
   The OTP MaxR/MaxT pattern (§6) is the missing guard.

4. **work-queue has no claim/owner record.** The fetch path is race-free via
   broker serialization (so no CAS needed — see §3), but the shared `_default`
   cursor means there's *no record of which consumer claimed which item*. For
   the supervisor (§7) to know "worker X is doing item 7," fetch should optionally
   stamp an owner. Today a worker that dies mid-task leaves no trace of what it
   had claimed; the item is simply gone past the cursor.

5. **Pipeline gating relies on the agent's prompt.** There is no
   `deliver_after`/typed-completion gate; "A done → mail B" lives in A's role
   prompt by convention. The broker already folds `Stop` events
   (`delivery.cpp` `scanEvents`) — the machinery to gate on a typed completion
   fact exists but isn't exposed as a delivery option.

### 9.2 Recommendations, ordered by payoff ÷ effort

| # | Move | Principle | Effort | Payoff |
|---|---|---|---|---|
| **1** | **Generalize `maybeAutoClear` into a triage table** `(signature, action, guard)`; add STUCK→nudge and high-CTX→`/compact` rows | Most escalations are mechanically fixable; human attention is the scarce resource. amux-proven. You already compute the signatures (`agent_status.h`) and CTX% (`maybeScanTokens`). | Med | **Very High** |
| **2** | **Add the gather/join barrier** using the existing `correlation` field + a `results-<corr>` topic; broker emits one pointer-bearing `gather-complete` on quorum/timeout | Gather is the missing half of fan-out; keep N results out of the coordinator's context (the dynamic-workflows split). Unlocks map-reduce *and* judge panels. | Med–High | **Very High** |
| **3** | **Add restart-intensity (MaxR/MaxT) guard** to any auto-recovery action; on exceed, stop healing + mail human | The OTP anti-thrash invariant. Without it, generalized triage (#1) can loop forever on a broken agent. Cheap insurance on a powerful feature. | Low | **High** |
| **4** | **Judge-panel / verify-and-converge over panes**: `broadcast` fan-out → `judge` role collects (via #2) → refutes via `mail` → iterate to convergence | Independent refutation is a cheap error signal; the durable-pane version is *more* debuggable than the headless one (human watches it resolve). Reuses existing primitives + #2. | Med | **High** |
| **5** | **Typed-completion pipeline gate** (`deliver_after=<corr>`): broker releases B's record only after observing A's `Stop` for that correlation | Substrate-enforced stage gates beat prompt-enforced ones; extends the `Stop`-fold the broker already does. The robust version of amux's `done_pattern`. | Med | High |
| **6** | **Specialist long-lived roles**: add `supervisor`, `judge`, `librarian` as `permanent` children; reflector as a headless cron | The durable-agent superpower is continuity; these roles only make sense because they persist. Each is a role prompt + a small mechanism. | Low–Med (per role) | High |
| **7** | **Optional owner-stamp on `work-queue` fetch** | Gives the supervisor visibility into in-progress claims and recovers orphaned work when a worker dies. | Low | Med |
| **8** | **Vote-weighting by reliability in the judge** (the one non-vapor ruflo idea) | Converged ensembles should weight agents whose past outputs survived refutation. Cheap refinement of #4. | Low | Med |

### 9.3 What to deliberately NOT do

- **Don't add SQLite CAS for work-queue claiming.** The broker already serializes
  fetch (§3); CAS solves a race you don't have. (SQLite still earns its place for
  *mutable coordination state* if you ever add per-claim ownership at scale — but
  not for the claim race itself.)
- **Don't make the durable agents headless to chase dynamic workflows.** That
  trades away the human-attachability that is the entire point. *Invoke* dynamic
  workflows as a bounded-fan-out tool from a durable agent; don't become one.
  (Same boundary the prior survey drew — restated for orchestration.)
- **Don't build a regex-over-scrollback pipeline gate (amux `done_pattern`).** You
  have typed `Stop` events; use them. The regex version is the thing you'd be
  *downgrading* to.
- **Don't take ruflo's consensus/neural framing at face value.** The Raft/
  Byzantine/Gossip topology vocabulary and "self-learning SONA/ReasoningBank" are
  marketing over an MCP-plugin CLI with no visible ML implementation. The *one*
  reusable kernel is reliability-weighted voting (#8).
- **Don't put orchestration plan/cursor/results in an agent's context window.**
  Keep them in broker state (you already do for delivery; do the same for
  gather). This is the deepest principle in the whole space — context is the
  scarce resource.

---

## Sources

**Anthropic dynamic workflows (the anti-claude-bus reference)**
- [Anthropic ships Opus 4.8 + dynamic workflows (1000-subagent cap) — MarkTechPost](https://www.marktechpost.com/2026/05/28/anthropic-ships-claude-opus-4-8-alongside-dynamic-workflows-and-cheaper-fast-mode-with-workflows-capped-at-1000-subagents/)
- [Opus 4.8 dynamic-workflow tool — TechCrunch](https://techcrunch.com/2026/05/28/anthropic-releases-opus-4-8-with-new-dynamic-workflow-tool/)
- [Dynamic Workflows in Claude Code: Complete Guide 2026 — claudefast](https://claudefa.st/blog/guide/development/dynamic-workflows)
- [Opus 4.8 release: effort controls, dynamic workflows — The New Stack](https://thenewstack.io/claude-opus-48-release/)

**Self-healing & scheduling (the durable-state reference)**
- [mixpeek/amux — watchdog, monitor triage loop, SQLite board, done_pattern](https://github.com/mixpeek/amux) (read locally: `scripts/watchdog.py`, `amux-server.py` `_run_schedule`/`_watch_schedule_response`/`_scheduler_loop`/monitor recovery rules ~2122–2440, `_pickup_next_board_task`)

**Supervision theory**
- [Erlang/OTP Supervisor Principles (restart strategies, MaxR/MaxT, child types)](https://www.erlang.org/doc/system/sup_princ.html)

**Orchestration topology & task allocation**
- [AdaptOrch: task-adaptive orchestration from DAG structure](https://arxiv.org/pdf/2602.16873)
- [5 multi-agent orchestration patterns that work — Digital Applied](https://www.digitalapplied.com/blog/multi-agent-orchestration-5-patterns-that-work)
- [Swarm vs mesh vs hierarchical — gurusup](https://gurusup.com/blog/agent-orchestration-patterns)
- [Decentralized adaptive task allocation (2025) — Nature Sci Reports](https://www.nature.com/articles/s41598-025-21709-9)
- [Auction-based agent interaction (cost-effective comms)](https://arxiv.org/html/2511.13193v1)
- [Negotiation & consensus for multi-agent LLMs — apxml](https://apxml.com/courses/multi-agent-llm-systems-design-implementation/chapter-3-agent-communication-coordination/agent-negotiation-consensus)

**Read skeptically**
- [ruvnet/ruflo — Queen-led topology, trust-scoring, "neural" claims (mostly vapor; reliability-weighting is the one kernel)](https://github.com/ruvnet/ruflo)

**Memory & learning (durable-agent superpower)**
- [CoALA — Cognitive Architectures for Language Agents (four-store memory)](https://arxiv.org/pdf/2309.02427)

**claude-bus source read for this doc**
- `src/broker.cpp` (RPC handlers, enqueue/pubsub cascade, fetch serialization, drop)
- `src/delivery.cpp` (`scanEvents`, `dispatchAgentInbox`, `escalate`, `scanRetries`, `maybeAutoClear`, `maybeScanTokens`)
- `src/topic_log.h` (v4 wire format, unused `correlation` field, cursors)
- `src/topic_registry.h` (topic kinds)
- `layouts/fleet.kdl` + `roles/{auri,comms,bast,kvothe,elodin}.md` (the roster)
