# claude-bus improvement roadmap

The single working document. Seven deep-dives (`docs/deep/*.md`) surveyed the
landscape and read our own source line-by-line; this folds all of it into one
prioritized, principle-first plan. Each item leads with the *mechanism or design
pressure* that justifies it, then the concrete change, a payoff÷effort score, and
a link to the deep doc that carries the full reasoning, source quotes, and code
references.

Read the deep docs for detail. This file is for deciding *what to do next*.

---

## The frame: what claude-bus actually is

Three findings recur across every deep-dive and should shape every decision:

1. **The moat is the standalone, out-of-context broker as system-of-record.**
   Anthropic's Agent Teams now ships durable attachable panes *and* a message
   bus — so the differentiator is no longer "panes + a bus." It is that our
   broker owns retry / ack / DLQ / audit / cursors as a process *outside* every
   agent's context: it survives a lead's compaction and costs zero context
   tokens, where Agent Teams messaging is context-coupled and bills tokens in
   both sender and receiver on every message. Protect that property. Cede
   spawn/lifecycle to native primitives where they're better; keep the broker
   for durable audited out-of-context delivery + the human gate.
   (`docs/deep/cc-config.md`)

2. **The load-bearing fragility is the TTY write.** A TUI owns its slave PTY;
   any outside write races the program's reads, the human's keyboard, and the
   line-editor mode. Every delivery bug in MEMORY (mid-stream dropped turn,
   broker wedge, detectMode scar tissue) traces to this. Nobody serious delivers
   through the TTY — Agent Teams and HCOM both use a filesystem inbox the agent
   *drains itself*. Moving delivery off the TTY is the highest-leverage
   structural move on the board. (`docs/deep/transport.md`)

3. **The broker is already 80% of an event-sourced system that hasn't named
   itself one.** Append-only fact logs, per-consumer cursors, idempotent record
   IDs, a boot-epoch fence — all present. What's missing is *exactness* (typed
   reducer, msg_id-keyed ACK/dedup) and *tests* (every pure function is
   untested). The design is already test-shaped; the cheapest high-leverage work
   is to make it honest about what it is. (`docs/deep/broker-internals-cpp.md`)

---

## The handful of highest-leverage moves

If you do nothing else, do these. They are the cluster where payoff is very-high
and they unblock the rest.

| # | Move | Why it leads | Deep doc |
|---|------|--------------|----------|
| **A** | Move agent-inbox delivery **off the TTY**: broker writes a FIFO/inbox file, a `UserPromptSubmit` (+`SessionStart`) hook drains it and emits `additionalContext`. | Kills all three delivery races at once, makes ~400 lines of screen-scraping irrelevant, keeps the pane for the human, and gives a clean process-exit completion signal that fixes the dedup hole. The single change three separate docs converge on. | transport §6/§6b, cc-config, comms-patterns |
| **B** | **`bus_core` static lib + unit tests** for the pure logic (`computeAxes`, `topic::parseFrom`, `json::parse`, tail-reader). | Turns the dropped-turn and broker-wedge *mysteries* into red tests. No new dependency; the code is already pure. Makes every later refactor safe. | broker-internals-cpp |
| **C** | **Turn on Claude Code OTel** (metrics+events) fleet-wide via the symlinked settings env block. | Consume the vendor's call-site instrumentation instead of re-deriving it; `query_source`/`agent.name` give per-subagent cost+token attribution for free. Zero C++. | observability, cc-config |
| **D** | **`SessionStart` injects per-agent `learnings.md`** via `additionalContext`. | The one structural gap that makes learning-over-time *possible*: identity + trace already hold, the read-path does not. Activates everything downstream in the memory track. | memory-learning, cc-config |
| **E** | **paneState TTL cache (200–500ms)** on the delivery loop. | The *actual* cause of the "delivery wedge" is a blocking forked `zellij dump-screen` (up to 5s) on the deadline-owning loop thread. A TTL cache collapses N forks/tick to ≤1. epoll would not fix this. | broker-internals-cpp, transport |

Note the dependency spine: **B unblocks safe iteration on everything**; **A and
E both attack the wedge** (E is the cheap stopgap, A is the structural cure — do
E first, it buys time for A); **D is the prerequisite for the entire memory
track**; **C is independent and free, do it immediately.**

---

## Tier 1 — Foundations (do now)

Cheap, structural, and unblock later work. Mechanism first.

### 1.1 — paneState TTL cache  ·  payoff high / effort low
**Principle:** unbounded-latency I/O must never run on the thread that owns a
deadline. Cache the forked-subprocess result with a 200–500ms TTL so a tick
makes ≤1 `dump-screen` call instead of N. This is the real fix for the delivery
wedge — epoll wouldn't touch it. *Fixes:* `src/rpc.cpp` loop starvation,
`src/pane.cpp kDefaultSubprocessTimeout`. → `docs/deep/broker-internals-cpp.md`

### 1.2 — `bus_core` static lib + unit tests  ·  payoff very-high / effort low
**Principle:** a pure function with a table test is a spec. Extract `computeAxes`,
`topic::parseFrom` (incl. truncated-tail), `json::parse` round-trip into a
testable lib; add a CMake test target (today tests are shell-integration only).
This is the leverage multiplier for every other change. → `broker-internals-cpp`

### 1.3 — Turn on OTel metrics+events fleet-wide  ·  payoff very-high / effort low
**Principle:** don't re-derive what the producer already emits. Env vars in the
already-symlinked `settings/claude-settings.json` + a local collector. **Guard:**
keep `OTEL_LOG_USER_PROMPTS` / `OTEL_LOG_TOOL_CONTENT` / `OTEL_LOG_RAW_API_BODIES`
default-off — the public-repo no-secrets rule extends to telemetry.
→ `docs/deep/observability.md`, `cc-config.md`

### 1.4 — Migrate state derivation off `extractField` onto `json_min`  ·  high / low
**Principle:** parse once into typed values; never substring-scan JSON. The
correct parser already ships in-tree and is used in `delivery.cpp`; the state
reader (`agent_status.cpp` `readAgents`/`extractField`) just predates it. A
payload-nested `"agent"`/`"event"` key can shadow the top-level one today.
→ `broker-internals-cpp`, `observability`

### 1.5 — Consumer-side dedup on the existing `msg_id`  ·  very-high / low
**Principle:** at-least-once is only safe with idempotent processing. We mint a
perfect key (`{sent_ms}-{sender}-{rand}`) and never use it for dedup. Record
last-acked msg_id per topic at the drain and refuse re-inject (~15 LOC).
Prerequisite-adjacent to move A; lands the idempotency floor either way.
→ `docs/deep/comms-patterns.md`, `transport.md`

### 1.6 — Tier CLAUDE.md: move broker spec + `bus help` to @-linked docs  ·  med / low
**Principle:** always-loaded context bills every turn of every agent forever;
reference material doesn't earn that slot, only every-turn rules do. Already
scored as violations in `docs/claude-md-conventions.md`. → `cc-config`

### 1.7 — Fix dead role frontmatter + false tool-restriction claim  ·  med / low
**Principle:** config must not claim guarantees the harness doesn't keep.
`agent-launch` strips `tools:`/`model:` frontmatter and passes only the body, so
`roles/comms.md`'s "your tools deliberately omit Edit and Write" is false. Either
parse+pass `--model`, or delete the claim. Pair with 2.5 (PreToolUse deny) for
the real enforcement. *Fixes:* `bin/agent-launch:194–230`, `roles/comms.md:176`.
→ `cc-config`

### 1.8 — Extract one `TailReader`  ·  med / low
**Principle:** one correct implementation beats three that drift. `scanEvents`,
`maybeScanTokens`, and `parseFrom` each reimplement resume-from-offset +
refuse-torn-tail. Note the `tellg` text-mode coupling: a `\r\n` in a payload
desyncs the byte count — treat the binary topic log as canonical, events.jsonl as
advisory. → `broker-internals-cpp`

---

## Tier 2 — High-leverage (next)

Bigger changes that pay for themselves. Several depend on Tier 1.

### 2.1 — Move agent-inbox delivery off-TTY (the FIFO/hook drain)  ·  very-high / med
**Principle:** kill the reader/keyboard/mode races at the source. Broker writes
`$STATE/inbox/<self>.log`; a `UserPromptSubmit` hook drains and emits
`additionalContext`; a `SessionStart` hook drains the boot backlog and re-arms a
Monitor for the idle-but-alive window (the hybrid drain — independent of the
agent taking a turn). **Must preserve the `[bus-attach]` presence sentinel** —
`additionalContext` interrupts a human turn just as much as a TTY write, so the
presence check moves *with* delivery. *Depends on:* 1.5 (dedup), 1.2 (tests to
prove it). Prototype on one agent. → `transport §6/§6b`, `cc-config`

### 2.2 — Bracketed-paste / single-raw-`write` as the *interim* TTY path  ·  high / low
**Principle:** until 2.1 lands fleet-wide, shrink the race window. Swap
`write-chars`→ zellij `paste` (bracketed-paste framed, multi-line-safe), or emit
`ESC[200~ + body + ESC[201~ + 0x0D` in one `write` call instead of
write-chars+send-keys (two forks → one). **Guard:** pin to claude agent panes
only — the markers ship unconditionally and would inject literal `ESC[200~`
garbage into a pane whose program has bracketed-paste off; and it must sit *after*
the readiness gate since raw `write` does zero mode adaptation. *Fixes:*
`pane.cpp:518–519`. → `transport §4.1`

### 2.3 — Make the ACK carry the `msg_id`  ·  very-high / med
**Principle:** the ACK join spans two logs (topics vs events.jsonl) correlated by
"oldest in-flight for this agent on the next `UserPromptSubmit`" — a human-typed
prompt or a misfired mid-stream injection acks the wrong record. Have the drain
hook emit `{event:bus-ack, msg_id}` so the join is by id, not position+time. This
is the direct fix for the mid-stream-dropped-turn ACK confusion. *Pairs with* 2.1
(the drain hook is where the ack is emitted). *Fixes:* `delivery.cpp:375–396`.
→ `comms-patterns`

### 2.4 — SessionStart injects per-agent `learnings.md`  ·  very-high / low-med
**Principle:** learning over time requires a read-path back into context — the
single structural gap. `additionalContext` is the documented deterministic
mechanism; today SessionStart runs only `log-event.sh` + `agent-register.sh` and
injects nothing. Without this, every distilled lesson is write-only. *Unblocks
the whole memory track.* *Fixes:* `settings/claude-settings.json:51–65`.
→ `memory-learning`, `cc-config`

### 2.5 — Turn `computeAxes` into a true fold + decouple from wall-clock  ·  high / med
**Principle:** state should be `fold(events)`, not `f(last_event)`. `readAgents`
overwrites `info.last` per line, so a planned-but-never-emitted tool call is
invisible — *exactly* why the mid-stream dropped turn escapes the state machine.
Carry open-tool + work-start in an accumulator; let a timerfd (not `age_s>30`
inline) decide *when* to escalate, which also makes the reducer pure and
testable. *Depends on:* 1.2 (the tests that make this safe), 1.4. *Fixes:*
`agent_status.cpp:396, ~250, 279, 309`. → `broker-internals-cpp`

### 2.6 — PreToolUse deny hook scoped to coordinator agents  ·  med / low
**Principle:** a must-always-fire rule belongs in a hook, not prose.
Deterministically deny `Edit`/`Write` for comms/auri so the "no file edits" rule
is real under fleet-wide `--dangerously-skip-permissions`. (Native Delegate mode,
Shift+Tab, is the platform version of this — worth evaluating as the long-term
answer.) → `cc-config`

### 2.7 — Token-anomaly detection in the delivery loop  ·  high / med
**Principle:** you already scan events every loop and own an escalation channel
(audit + inbox-human). A `maybeDetectAnomaly()` sibling to `maybeScanTokens`
tracks token deltas / llm-call counts between prompt turns and reuses the
`maybeAutoClear` escalation path — passive scanning becomes active budget
protection. *Depends on:* OTel (1.3) gives cleaner inputs, or runs on the
existing transcript scan. → `observability`

### 2.8 — Statusline sidecar for authoritative ctx% + cost  ·  high / low
**Principle:** don't re-derive a number the producer computes and will hand you.
Claude Code's statusline payload carries real `context_window.used_percentage`
and window size, eliminating `maybeScanTokens`'s guessed denominator and the
hard-coded `>200000 ? 1M : 200K` two-tier hack, and adds `cost_usd` +
`rate_limits`. *Fixes:* `delivery.cpp:925–990`. → `observability`

### 2.9 — Typed `Event` struct parsed once  ·  high / med
**Principle:** parse structured data at the boundary, once. Three duplicated
`extractField`/`extractStr` scanners with divergent escape handling
(`agent_status` handles `\n\r\t`, `sub_monitor`'s "naive" one doesn't) are a
silent-misparse class. This is the prerequisite for putting token data into the
replayable event stream. *Subsumes* 1.4; *enables* 2.7, 4.x trace work. *Fixes:*
`agent_status.cpp:29–55`, `sub/sub_events.cpp:43–69`, `sub/sub_monitor.cpp:136–148`.
→ `observability`, `broker-internals-cpp`

---

## Tier 3 — Coordination & resiliency hardening

The broker reinvented JetStream's at-least-once topology independently; these
make it exact and crash-safe. Most depend on the Tier-1/2 floor.

### 3.1 — Generalize `maybeAutoClear` into a (signature, action, guard) triage table  ·  very-high / med
**Principle:** most escalations are mechanically fixable (amux-proven). We already
compute the signatures (`agent_status.h`) and CTX% (`maybeScanTokens`); make
recovery rules *data*, not duplicated control flow. Add STUCK→nudge and
high-CTX→/compact rows. Reserves inbox-human for genuine novelty. *Fixes:*
`delivery.cpp:805–905`. → `docs/deep/orchestration.md`

### 3.2 — OTP-style restart-intensity (MaxR/MaxT) guard on every auto-recovery  ·  high / low
**Principle:** without an anti-thrash invariant, a generalized triage table
(3.1) can nudge/respawn a genuinely broken agent forever, burning tokens. On
exceed: stop healing, mail human. Cheap insurance on a powerful feature.
*Depends on:* 3.1. → `orchestration`

### 3.3 — Atomic claim + documented modes for work-queue  ·  high / med
**Principle:** `fetch` silently flips between load-balancing (shared `_default`
cursor) and fan-out (per-`--consumer` cursors); single-assignment relies on the
single-threaded RPC loop, not the data model. Add a claim/status field with
changes>0 CAS semantics; document both modes. *Fixes:* `broker.cpp:609–658`.
→ `comms-patterns`, `orchestration`

### 3.4 — Generalize in-flight tracker into a real PEL with idle-reclaim  ·  high / med
**Principle:** unify the in-flight map, blocking-op map, and retry timers under
one `(topic, consumer, msg_id, claimed_at, delivery_count)` model. A single
XAUTOCLAIM-style reaper redelivers push kinds and releases pull kinds idle past
AckWait — gives work-queue crash-recovery for free (today a fetched-then-crashed
consumer loses the item; cursor already advanced, no PEL entry). **Two knobs:**
`claimed_at` (idle-reclaim, near-unbounded) + `delivery_count` (poison DLQ, cap
3) so crash-recovery and poison-detection stop sharing one threshold. *Depends
on:* 1.5, 2.3. *Fixes:* `broker.cpp:650–654`. → `comms-patterns`

### 3.5 — NATS-style escalating backoff in `scanRetries`  ·  med / low
**Principle:** flat `now+ackTimeoutMs` hammers a wedged agent 3× at fixed
cadence; 1×/2×/4× spaces redelivery. *Fixes:* `delivery.cpp:762–763`.
→ `comms-patterns`

### 3.6 — NAK event channel  ·  med / low
**Principle:** the cooperative complement to exact-id ACK (2.3). Emit
`{event:bus-nak, msg_id}` on blocking-op/not-ready so the broker re-dispatches at
the *next idle boundary* deliberately, instead of burning three blind 60s
ackTimeoutMs cycles. *Depends on:* 2.3. → `comms-patterns`

### 3.7 — Per-topic depth cap + named full-policy per kind  ·  med / low
**Principle:** `max_record_bytes` caps a *record*, not a *queue*; work-queue and
audit grow unbounded. Name the policy each kind implements: inbox=one-in-flight,
work-queue=reject-on-full, blackboard=drop-all-but-latest, audit=age-trim. Keep
retention a *step inside the existing tick* — do not build a retention daemon.
*Fixes:* `topic_registry.h:52`, `topic_log.h:53`. → `comms-patterns`

### 3.8 — Active replay-on-restart in the SessionEnd recovery path  ·  high / med
**Principle:** our SessionEnd handler releases in-flight without advancing the
cursor (*passive* replay); make it *active* like amux — re-send on respawn, gated
by the idempotency key (1.5). *Depends on:* 1.5, ideally 2.1. → `transport`

### 3.9 — `bus msg redrive` (DLQ as a loop, not a dead end)  ·  med / low
**Principle:** `escalate` appends to audit+inbox-ops then advances the cursor —
the dead message is only readable as an obituary. Re-enqueue by msg_id with fresh
attempt count makes escalation recoverable. → `comms-patterns`

### 3.10 — Per-protocol guarantee selection  ·  med / low
**Principle:** name the guarantee per protocol-type rather than letting it emerge
from the blocking-op state machine. Idempotent payloads → at-least-once+dedup;
destructive commands (`/clear`, `/compact`) → at-most-once-preferred (running
twice is worse than not at all). Ties into typed envelopes. → `comms-patterns`

---

## Tier 4 — Orchestration & learning (the new capabilities)

This is where the durable-pane substrate becomes a platform. Depends on the
floor being solid.

### 4.1 — Gather/join barrier on the unused correlation field  ·  very-high / high
**Principle:** gather is the missing half of fan-out, and the v4 wire format
already reserves a 16-byte correlation field nothing reads. A `results-<corr>`
topic where the broker emits one pointer-bearing `gather-complete` on
quorum/timeout keeps N results *out of the coordinator's context* (the
dynamic-workflows context/coordination split). Unlocks map-reduce *and* judge
panels with one primitive. *Fixes:* `topic_log.h:59,69`. → `orchestration`

### 4.2 — Nightly reflection → per-agent `learnings.md` (or broker-drained continuous)  ·  high / med
**Principle:** the learning mechanism *is* the episodic→semantic distillation
function; periodic is the cheapest robust trigger. Transcripts (incl. thinking
blocks) are a complete per-agent corpus — headless `claude -p` reads them, no new
plumbing. *Better framing:* events.jsonl/topic logs ARE a queue and the broker IS
the worker — shell out to `claude -p` for near-real-time capture instead of 24h
latency. **Design the prune/consolidate path WITH the write path** to avoid
Reflexion's unbounded-buffer bloat. *Depends on:* 2.4 (the read-path).
→ `memory-learning`

### 4.3 — Promotion ladder: episodic → per-agent learnings → roles/*.md → CLAUDE.md  ·  high / low
**Principle:** memory scope must match identity scope; lessons flow upward on
recurrence + survival. The per-agent tier absorbs churn so CLAUDE.md stays under
its rule ceiling. The `roles/*.md` read-path *already exists*
(`--append-system-prompt-file`) — only the write/accrete side is missing.
*Depends on:* 2.4, 4.2. → `memory-learning`

### 4.4 — Specialist long-lived roles (supervisor, judge, librarian)  ·  high / med
**Principle:** the durable-agent superpower is continuity — these roles only make
sense *because* they persist, observe, and accumulate across sessions (ephemeral
fan-out structurally cannot). The librarian unifies reflection + cross-agent
pattern promotion + triage of novel failures. *Depends on:* 4.1 (judge needs
gather), 4.2 (librarian does reflection). → `orchestration`, `memory-learning`

### 4.5 — Typed-completion pipeline gate (`deliver_after=<corr>`)  ·  high / med
**Principle:** substrate-enforced stage gates beat prompt-enforced ones. Broker
releases stage B only after observing A's `Stop` for that correlation — the
robust version of amux's regex `done_pattern`, extending the Stop-fold
`scanEvents` already does. *Depends on:* 2.5 (the fold), 4.1 (correlation
plumbing). *Fixes:* `delivery.cpp:231–399`. → `orchestration`

### 4.6 — Judge-panel / verify-and-converge over panes  ·  high / med
**Principle:** independent refutation is a cheap error signal, and the
durable-pane version is *more* debuggable than headless — the human watches
disagreement resolve live. Reuses broadcast + inbox + mail. *Depends on:* 4.1,
4.4. → `orchestration`

### 4.7 — Wire the inert PreCompact hook to checkpoint decisions  ·  med / low
**Principle:** distill before destruction — the moment before a compact is when
episodic detail is richest and about to vanish (same event as MemGPT's
memory-pressure interrupt). The hook need only snapshot to a file; the cron (4.2)
distills later. *Fixes:* `settings/claude-settings.json:92–102`.
→ `memory-learning`, `cc-config`

### 4.8 — `bus learnings append` model-driven write verb  ·  med / low
**Principle:** commit a lesson at the *moment of insight* (richest context)
rather than reconstructing it after-the-fact by a reflector. Layered on the
deterministic floor (4.2) for coverage. → `memory-learning`

---

## Tier 5 — Exploratory (later / contingent)

Defer until a measured need appears. Listed so the option is on record.

- **`bus trace` live causal span-tree viewer** (high payoff / high effort) — the
  unique-to-claude-bus third observability plane; a pane shows the
  interaction→llm→tool→subagent tree *as it happens*. Depends on beta OTel
  traces + 2.9. → `observability`
- **Bound `TopicLog::peek` to a windowed pread** (med/med) — read work should be
  O(records returned), not O(log size); the `body` RPC dumps every topic's whole
  log to resolve one id. Do when the wedge profile shows it. *Fixes:*
  `topic_log.cpp:306`, `broker.cpp:571`. → `broker-internals-cpp`, `comms-patterns`
- **CQRS read-model index** (med/high) — `body`/`drop`/`state` RPCs are O(total
  bus bytes) on the append-log substrate. Contingency; pairs with the peek bound.
  → `comms-patterns`
- **Keyed, compacting blackboard** (med/med) — Kafka-changelog over the
  append-log: per-key last-value-wins + tombstones + periodic rewrite. Today
  superseded values accumulate forever. → `comms-patterns`
- **signalfd in an epoll reactor** (med/med) — eliminates the
  async-signal-safe + atomic gStopFlag + EINTR dance; composes with an epoll
  migration. Don't sequence epoll strictly last if RPC robustness against slow
  clients matters as much as delivery latency. → `broker-internals-cpp`
- **importance×recency SQLite retrieval** for learnings (med/med) — only when
  inject-all outgrows the context budget; prefer scored-SQLite (poignancy +
  last_confirmed) over embeddings (no model dependency, no vector staleness). Do
  NOT do preemptively. → `memory-learning`
- **Reserve headless `claude -p --input-format stream-json`** for cron / judge /
  reflection jobs only (low/med) — race-free result-ACK channel, but it *costs
  the pane*; never headless-ify durable fleet agents (deletes the moat).
  → `transport`
- **Overstory's single WAL'd mail table** — contingency replacement for the
  flat-file in-flight tracker *only if* the `$STATE/in-flight/` directory-scan
  shows in the broker-wedge profile. Flagged, not recommended now. → `transport`
- **Cost-per-skill attribution + golden-signals monitor columns** — `skill.name`
  is inherited by subagents, enabling per-coordination-pattern cost rollups;
  api_error rate + retry count are the highest-value currently-blind signals.
  → `observability`
- **`bus topic verify NAME`** (low/low) — walk a log asserting strictly-increasing
  ids + valid record_len chain; sequence-gap/corruption detection the append-log
  substrate gives nearly for free. → `comms-patterns`
- **Restartable coordinator** — write the decomposition to a durable
  `plan-<corr>` blackboard cell so killing+respawning the coordinator pane
  reconstructs "what's outstanding" from broker state; the highest expression of
  "context is the scarce resource." Depends on 4.1. → `orchestration`

---

## Cross-cutting flaws folded in as fix items

These flaws from the deep-dives are tracked above rather than separately; pointer
table for traceability:

| Flaw | Tracked as | Ref |
|------|-----------|-----|
| `sendToPaneSafe` mode=unknown false-defer on scrolled pane | 2.1 (off-TTY removes screen-scrape) | `pane.cpp:557` |
| Synchronous zellij subprocess on loop thread (the wedge) | 1.1 (TTL cache) + 2.1 | `pane.cpp:33–59`, `delivery.cpp dispatchTuiCommands` |
| `detectMode` heuristic fragility / scar tissue | 2.1 (structured off-TTY ACK) | `pane.cpp:354–366,433–436` |
| flock doesn't guard the human keyboard | document in 2.1; presence sentinel is the real guard | `dispatch.cpp FlockGuard`, `delivery.cpp:62–89` |
| i+Ctrl-U normalize mutates human draft | 2.1 eliminates; 2.2 shrinks the window | `pane.cpp:592–606` |
| State is f(last_event) → dropped turn invisible | 2.5 (true fold) | `agent_status.cpp:396` |
| Wall-clock thresholds make reducer untestable | 1.2 + 2.5 | `agent_status.cpp:279,309` |
| `extractField` flat substring JSON scan | 1.4 → 2.9 | `agent_status.cpp:29,368` |
| No unit tests for pure logic | 1.2 | `tests/bus-itest.sh`, `CMakeLists.txt` |
| Token watcher dedup/summability | 2.8 (statusline) + 2.9 | `delivery.cpp:959–980` |
| Context-window denominator hack | 2.8 | `delivery.cpp:925–990` |
| OTel content gates could leak prompts | 1.3 guard | `settings/claude-settings.json` |
| No memory read-path / no reflection / scope mismatch | 2.4, 4.2, 4.3 | `settings/claude-settings.json:51–65` |
| PreCompact / roles inert | 4.7, 4.3 | `settings/claude-settings.json:92–102`, `agent-launch:194–210` |
| ACK two-log join / no dedup | 1.5, 2.3 | `delivery.cpp:375–396,744–766` |
| work-queue claim race / crash-recovery | 3.3, 3.4 | `broker.cpp:609–658,650–654` |
| Flat retry / no backpressure / blackboard accretion | 3.5, 3.7 | `delivery.cpp:762`, `topic_registry.h:52` |
| Unused correlation field / no gather | 4.1 | `topic_log.h:59,69` |
| maybeAutoClear one-off / no anti-thrash | 3.1, 3.2 | `delivery.cpp:805–905` |
| No typed-completion pipeline gate | 4.5 | `delivery.cpp:231–399` |
| Dead role frontmatter / false tool claim | 1.7, 2.6 | `agent-launch:194–230`, `roles/comms.md:176` |
| CLAUDE.md bloat | 1.6 | CLAUDE.md |

---

## Dependency map (quick reference)

```
1.2 (tests) ───────────────► makes safe: 2.5, 2.1, everything
1.4 (json_min) ──► 2.9 (typed Event) ──► 2.7, 4.x trace
1.5 (dedup) ──► 2.1, 2.3, 3.4, 3.8
2.1 (off-TTY) ──► 2.3 (ack hook) , 3.8 ; obsoletes 1.1/2.2 stopgaps
2.3 (msg_id ack) ──► 3.4, 3.6
2.4 (SessionStart inject) ──► 4.2 ──► 4.3, 4.4
4.1 (gather barrier) ──► 4.4 (judge), 4.5 (pipeline gate), 4.6, restartable-coordinator
3.1 (triage table) ──► 3.2 (anti-thrash)
1.1 + 1.3 + 1.6 + 1.7: independent — do immediately
```
