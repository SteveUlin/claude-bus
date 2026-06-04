# Workpaths proposal (generative) — the broker as a system-of-record for work and causality

Author: auri (hub). Status: proposal for sulin's direction. Generative, not a
sequencing of a fixed list: sulin's items (reliability, MPMC queues, task-DAG,
renderer, cull, auto-compaction, causal-manifest) are treated as **seeds**; this
adds new capabilities he didn't name, informed by prior-art research (beads;
task-DAG engines; MPMC queues; worker pools; auto-compaction; and a cross-domain
capability scan). Still plan-only.

---

## 0. The realization

The substrate's next leap is **not more delivery hardening**. It's the broker
graduating from *system-of-record for MESSAGES* to *system-of-record for WORK
and CAUSALITY*. Almost every surprising capability below falls out of
**repurposing assets that already exist or lie dormant** — not new daemons:

- The **`correlation` u128 field, read by nothing today** → causality:
  trace-trees, threaded conversations, request/reply, and sulin's
  causal-manifest (right-sized) all fall out of stamping + propagating one id.
- A **dependency-graph tracker** (beads-native, an append-log projection, **no
  Dolt**) → persistent queryable work-memory → a `ready` query → auto-routing →
  orchestration-as-a-consumer. The tracker is the FOUNDATION sulin asked for;
  the work-queue, DAG execution, cull, and renderer are all *consumers/views*
  of it.
- The **append-only log + ack/in-flight machinery** → replay, dead-letter,
  idempotency, SLA timers, dead-man switch → a self-reporting, unattended-safe
  fleet.

So instead of seven separate features there are **three capability pillars**,
each mostly repurposing existing machinery, and sulin's named items slot in as
members or consumers. The reliability tier is the floor all three stand on
(every time-based capability needs the trustworthy monotonic clock — see §4).

### The keystone: the coordination graph (Pillars A + B unify)

The sharpest reframe: **the task graph is not only a thing sulin/agents
explicitly define — it is an emergent byproduct of the hub coordinating.** Every
dispatch auri makes is already a node with dependencies and a live status; every
stamped message (Pillar A) is already an edge. So the work-graph tracker
(Pillar B) need not be hand-populated — *it falls out of auri doing its job.*

This collapses three of sulin's asks into one artifact:
- **explicit tasks** (beads sulin/agents create) and **emergent tasks** (auri's
  dispatches) are the *same node type in the same graph*;
- **causality** (Pillar A's trace/parent stamps) is *how edges get created
  automatically* as the hub coordinates;
- the **graph is the live system-of-record for all in-flight fleet work**,
  inspectable and renderable at any moment; the jj-renderer is just its view.

That is the unification: the task-DAG reframed as **the hub's coordination
state, made first-class and inspectable.** It bridges the explicit-DAG ask and
the durable-project-tracker ask — both land in one graph — and it means the
tracker is *never stale or hand-maintained*, because coordinating *is*
populating it. Pillar A and Pillar B below are the two halves of this one thing.

---

## Pillar A — Causality (activate the dormant `correlation` field)
*Surprising, cheapest, highest unlock-per-line. Directly attacks the lived
pain that triggered this thread: comms surfaced a STALE question because nothing
records message causality.*

- **Trace-id + `parent_msg_id` on every record.** Propagate one id through a
  chain of agent-to-agent messages → a span tree. `bus trace <id>` reconstructs
  "who triggered whom" from one prompt. *Cheap — the field exists.*
- **Threaded conversations.** Group by trace-id; `bus thread <id>` reads an
  interleaved multi-agent exchange in order. *Cheap on top of trace-id.*
- **Request/reply (`bus ask <agent> <q>`).** Sender blocks-or-polls on a reply
  correlated to its msg_id — kills the dump-screen dance and most of the human
  relay burden. *Medium — needs a reply topic + correlation join.*
- **Causal-manifest, right-sized (sulin's idea, weighed).** Full vector clocks
  are heavy and of uncertain value. The *value* is staleness detection, and a
  lightweight **per-message "context-cursor"** delivers it: stamp each message
  with the sender's last-seen offset per relevant topic at construction time. A
  question constructed before its answer arrived is then detectable (its stamp
  predates the answer's offset) → flag/suppress instead of surfacing raw.
  Queryable from the broker, not always shown — exactly sulin's framing. Full
  happens-before vector clocks: **deferred until measured need.**

## Pillar B — The work-graph tracker (beads-native foundation)
*The durable, queryable PROJECT TRACKER sulin wants — which doubles as the
orchestration substrate. Reference: beads (Steve Yegge, github.com/steveyegge/
beads). We steal its model and skip its worst trap.*

- **`issue-graph` topic kind = append-log of bead mutations + an in-memory
  graph projection** rebuilt from the log on broker start. A bead =
  `{id, title, body, status∈{open,in_progress,closed}, priority, assignee,
  labels[], deps:[{type, target}]}`. The dormant **`correlation` u128 is the
  bead id** — one identity space for log records and issues.
- **`bus ready` = open ∧ no incomplete `blocks` dependency (transitive)**,
  computed deterministically in C++. This is the single load-bearing query.
  Only `blocks` gates readiness; `parent`/`related`/`discovered-from` are
  non-gating metadata. *Why it matters: it moves "what can I work on now" OUT
  of the agents' LLM reasoning — they stop re-deriving order every turn (token
  savings) and stop picking blocked work.*
- **Atomic claim → the existing in-flight/ack tracker.** beads' `--claim`
  (assignee + in_progress in one op) maps onto our msg_id in-flight machinery;
  reuse the race-safety we already have. `discovered-from` records provenance
  when a working agent spawns a follow-up — cheap, high-value for debugging
  agent pipelines.
- **The fusion — auto-routing (the surprise):** newly-ready beads auto-enqueue
  and the broker routes them to the assigned lane's inbox (label→role). **The
  tracker IS the work-queue producer; pipelining/DAG-execution is a consumer;
  the jj-renderer is its view.** This makes one artifact simultaneously a
  durable project tracker sulin queries AND the orchestration engine.
- **Skip Dolt entirely (the biggest design win).** beads needs git-like
  merge (SQLite→Dolt) only because it's file/git-*distributed*; our broker is a
  single authoritative process, so the entire reason Dolt exists doesn't apply.
  We get the queryable dependency graph **without** the merge-conflict tax
  Yegge openly calls his worst trap — the append-only topic log already is the
  durable source of truth.
- **This reframes four of sulin's seeds into one coherent thing:** MPMC queue
  (R3/R4) becomes the durable claim layer *under* the tracker; the task-DAG
  *is* the tracker; the renderer is its view; pipeline-gate (O7) is a consumer.

## Pillar C — Self-reporting, unattended-safe fleet
*Makes unattended running real — the Path-1 reliability floor, generatively
expanded past what sulin named.*

- **Named floor:** R1 triage auto-recovery + R2 restart-intensity guard
  (OTP MaxR/MaxT; never ship R1 without R2) + monotonic-clock/suspend-fix.
  Auto-compaction folds in here as R1 triage rows (sulin's Q3 answer:
  high-CTX | stale-cache → /compact; the cold-cache free-ride rule is highest
  value — compaction cost is *inversely* correlated with idle time).
- **NEW generative members:**
  - **Dead-man switch** — an agent that should be working but emits no events
    for N min fires an alert. Catches the known silent-wedge / mid-stream
    dropped-turn failure when sulin isn't watching. *Cheap (presence + events
    gap).*
  - **Standing-query digest (`bus digest`)** — saved predicates ("any STUCK",
    "idle >30m while ready-work exists", "token >80%") rolled into a periodic
    narrative the broker mails the relay. **The fleet reports itself**; sulin
    re-attaches to a paragraph, not 1000 JSONL lines, and stops polling
    `bus state`. *Cheap — predicates over data computeAxes already folds.*
  - **Dead-letter topic + `bus replay <msg_id>`** — exhausted-retry records
    land on a durable replayable topic instead of only an inbox-human obituary;
    recovery is one command. *Cheap (escalation + append-log already exist).*
  - **Idempotency keys on enqueue** — dedup by a caller key so a retried send
    can't double-deliver; the floor automation + request/reply lean on.
  - **SLA timers on queued work** — per-record deadline; unpicked/​unacked past
    N min escalates or reassigns. Reuses the ack-timeout machinery.
  - **Anomaly detection** — token-burn spike / retry storm / restart loop →
    quarantine the agent (OTP circuit-breaker). *Medium — needs baselines.*

## Pillar D — Broker-infra / perf (the engine room)
*sulin's added lane. Not a capability sulin sees — the architecture that lets
every other pillar scale without slowing intake, and it fixes a known
fragility.*

- **Decouple intake from processing (the seed).** Today the RPC accept-drain
  loop and the processing+delivery tick share one thread, and the tick
  effectively advances *on RPC traffic* (the known RPC-driven-tick property:
  delivery can stall when no client is polling). Introduce a **threadsafe queue
  between intake and processing** — intake thread(s) accept RPCs and enqueue;
  ONE processing thread drains and runs the tick/delivery state machine. Intake
  never blocks on a slow processing step (a TTY write, a big log scan), so the
  broker stays responsive under load.
- **Self-driven processing tick (the bigger win inside the seed).** With
  processing on its own thread, drive its tick from a **timerfd/epoll reactor**,
  not from incoming RPC traffic. This *fixes the RPC-driven-tick fragility
  directly*: PEL idle-reclaim (W12), SLA timers (W21), dead-man switch (W17),
  retry escalation, and compaction timing all currently need something to poke
  an RPC to advance — a self-driven tick makes them fire reliably whether or not
  a client is talking. D8 Part B already did this for escalation via one
  timerfd; this generalizes it to the whole loop.
- **The non-negotiable constraint:** keep the **processing side
  single-consumer.** The MPMC claim (W12), cursor advances, and the graph
  projection (W6/W10) rely on the atomicity that today's single-threaded RPC
  gives for free. The design must be intake-threads → queue → *one* processing
  thread, so atomicity is preserved by construction. This is a producer/consumer
  (reactor) split, **not** "make the broker multithreaded."
- **Why early:** this is a **scaling prerequisite** for the capability pillars,
  not a nice-to-have. Every pillar adds processing-side cost (ready-query, graph
  projection, digests, anomaly scans); without the split, heavier processing
  directly slows intake. Do the decouple before the processing side gets heavy.
  Design-doc first — it touches the RPC/tick/restart spine, where elodin's
  "design before code on restart/ack/cursor semantics" rule applies.
- **Trap:** an unbounded intake queue lets a fast producer outrun processing and
  blow memory — bound it + define a backpressure policy; govern any queue-age
  timers with the same monotonic clock as W1.

## Cross-pillar ergonomics (smaller, opportunistic)
- **Time-travel / replay-to-state** — "show the fleet's event history at T" and
  step through it; post-mortem any unattended run. *Cheap — logs already
  support it; needs a reader.* (Also the renderer's time-travel mode.)
- **Undo-before-delivery (`bus undo <msg_id>`)** — auto-routed/off-TTY messages
  sit in a pre-delivery queue that already gates them; mark one superseded
  before it lands. Safety rail for automation.
- **Shared blackboard as boot-memory** — promote the blackboard kind to a typed
  "decisions / what-we-learned" store every (re)launched agent reads on boot
  (CrewAI-style shared memory).
- **Verify-and-converge reviewer role** — on task completion auto-route the diff
  to a reviewer; mark done only on approval. A quality gate that's a natural
  consumer of the tracker + request/reply.

---

## 3. Curated "surprise" shortlist (my recommendation)

If I had to pick the highest-leverage, most-surprising, cheapest set — the three
that turn the fleet from *"messages I supervise"* into *"a self-reporting
work-graph that routes and explains itself"*:

1. **Activate the `correlation` field** → trace + threads + request/reply +
   right-sized causal-manifest. One dormant field unlocks four capabilities and
   fixes the exact stale-question pain.
2. **Beads-native work-graph tracker with auto-routing** (no Dolt). The
   foundation sulin named — and it absorbs MPMC/DAG/renderer/pipeline into one
   coherent layer where ready-work routes itself to champions.
3. **Self-reporting layer: dead-man switch + `bus digest`.** Makes unattended
   running real and cuts the relay burden; both cheap.

---

## 4. Sequence, champions, net-new vs roadmap

**Sequence (reliability floor is a structural dependency, not just a priority —
trace timing, PEL reclaim, cull, compaction TTL-race all read a clock the
suspend-fix must first make trustworthy):**

- **Phase 1 — Reliability floor (Pillar C core):** monotonic-clock/suspend-fix
  → R1 triage (+R2). Auto-compaction lands as R1 rows. [elodin]
- **Phase 1b — Broker-infra (Pillar D, design-first):** intake/processing
  decouple (W23) → self-driven tick (W24). A scaling prerequisite — land before
  the Phase-3 processing load grows; de-risks every time-based capability by
  making the tick fire independent of RPC traffic. [elodin]
- **Phase 2 — Causality (Pillar A):** activate `correlation`/`parent_msg_id` →
  `bus trace`/`bus thread` → request/reply → context-cursor staleness. Cheap,
  high-leverage, and the claim/causality groundwork the tracker reuses.
  [elodin protocol; kvothe trace/thread viewers]
- **Phase 3 — Work-graph tracker (Pillar B):** `issue-graph` kind + `bus ready`
  + atomic claim + auto-routing. Then orchestration consumers (DAG execution,
  O1 gather/join, O7 pipeline-gate) and the jj-renderer as its view. [elodin
  foundation; kvothe renderer]
- **Self-reporting + ergonomics (Pillar C rest):** dead-man switch, `bus
  digest`, dead-letter+replay, idempotency, SLA timers — slot in as slack
  allows; several are independent low-effort wins schedulable any time.

**Parallel tracks (keep bast + kvothe off elodin's serial broker spine):**
- **bast:** explicit cull verbs now (sulin's Q2: defer auto-sizing), agent
  lifecycle, dead-man-switch hook wiring.
- **kvothe:** `bus trace`/`bus thread` viewers, the jj-style tracker renderer
  (prototype vs a mock graph now), `bus digest` rendering.

**Champions** (role-authoritative): **elodin** — correlation propagation,
tracker topic-kind + ready-query + claim, reliability internals, digest/anomaly
signals (the broker-internal spine, and the bottleneck). **kvothe** — trace/
thread/tracker/digest viewers. **bast** — cull verbs, lifecycle, hook wiring.

**Net-new vs roadmap:** *On-roadmap:* R1, R2, R5, R3/R4 (now the tracker's claim
layer), O1, O7, suspend-fix (~D8). *Net-new:* the work-graph tracker + `bus
ready` + auto-routing; the causality pillar (trace/thread/ask/context-cursor —
O1 is the only roadmap piece); and the self-reporting members (dead-man switch,
digest, dead-letter/replay, idempotency, SLA timers, anomaly — some echo the
deferred X-tier).

**Flags (accepted by sulin):** elodin is the bottleneck (Phases 1–3 broker-
internal, serial; hub holds land-order). R-tier routes to elodin (its code is
`delivery.cpp`/`broker.cpp`), not kvothe as the roadmap's lane column says.

---

## 5. Workpath table (row-per-item)

| ID | Short name | Scope (one line) | Phase | Champion | Net-new vs roadmap | Depends-on | Effort |
|----|-----------|------------------|-------|----------|--------------------|-----------|--------|
| W1 | suspend-fix + monotonic clock | Verify D8 coverage; monotonic guard on state-age reads so a resume clock-jump can't fire false STUCK | 1 Reliability | elodin | roadmap (~D8) | — | low |
| W2 | R1 triage auto-recovery | Data-driven signature→action→guard table; STUCK→nudge, high-CTX→/compact | 1 Reliability | elodin | roadmap (R1) | W1 | med |
| W3 | R2 restart-intensity guard | OTP MaxR/MaxT anti-thrash so recovery can't churn a broken agent forever | 1 Reliability | elodin | roadmap (R2) | W2 | low |
| W4 | auto-compaction rows | Fold into R1: high-CTX \| cold-cache-free-ride → /compact (compaction cost inversely correlated with idle) | 1 Reliability | elodin | net-new signal / roadmap R1 | W2 | low |
| W5 | correlation activation | Stamp + propagate trace-id + parent_msg_id on every record | 2 Causality | elodin | net-new (enables O1) | — | low-med |
| W6 | coordination-graph projection | Emergent graph: every dispatch a node, trace/parent edges, live status from ack/state — the inspectable hub-state keystone | 2 Causality | elodin (proj) + auri (populates) | net-new | W5 | med |
| W7 | bus trace / bus thread | Span-tree + threaded-conversation readers over the graph | 2 Causality | kvothe | net-new | W5,W6 | med |
| W8 | request/reply (bus ask) | Correlated RPC; sender awaits a reply — kills dump-screen + most relaying | 2 Causality | elodin | net-new | W5 | med |
| W9 | context-cursor staleness | Per-message last-seen-offset stamp; broker flags a question built before its answer | 2 Causality | elodin | net-new (right-sized causal-manifest) | W5 | low-med |
| W10 | issue-graph kind + bead model | Append-log of bead mutations + in-memory projection; bead id = correlation; unifies explicit beads + emergent nodes (W6); no Dolt | 3 Tracker | elodin | net-new (beads) | W6 | med-high |
| W11 | bus ready query | open ∧ no incomplete `blocks` dep, transitive, deterministic C++ | 3 Tracker | elodin | net-new | W10 | low-med |
| W12 | MPMC claim + PEL layer | Redis-PEL: claim via in-flight tracker (owner+claimed_at), idle-reclaim; durable layer under the tracker | 3 Tracker | elodin | roadmap (R3,R4) | W1,W11 | med |
| W13 | auto-routing | Ready beads auto-enqueue → route to the assigned lane's inbox (label→role) | 3 Tracker | elodin | net-new | W11,W12 | med |
| W14 | jj-style graph renderer | Render the coordination/work graph by node-state + dep edges; the inspectable view (prototype vs mock early) | 3 Tracker | kvothe | net-new | W6,W10 | med |
| W15 | orchestration consumers | O1 gather/join, O7 pipeline-gate, DAG execution as consumers of the tracker | 3 Tracker | elodin | roadmap (O1,O7) + net-new exec | W10-W13 | high |
| W16 | explicit cull verbs + lifecycle | spawn/cull verbs, drain-before-cull, restart-intensity guard; defer auto-sizing (sulin Q2) | parallel | bast | net-new | — | med |
| W17 | dead-man switch | Alert when a should-be-working agent goes event-silent N min (catches silent-wedge) | parallel | bast (hook) + elodin (detect) | net-new | W1 | low |
| W18 | bus digest (standing-query) | Saved predicates → periodic narrative to the relay; the fleet reports itself | parallel | elodin (eval) + kvothe (render) | net-new | — | med |
| W19 | dead-letter + bus replay | Exhausted-retry → replayable topic; recover by id (not just an obituary) | parallel | elodin | net-new (~X2) | — | low |
| W20 | idempotency keys | Dedup enqueue by caller key so a retried send can't double-deliver | parallel | elodin | net-new | — | low |
| W21 | SLA timers on queued work | Per-record deadline → escalate/reassign if unpicked/unacked | parallel | elodin | net-new | W12 | low |
| W22 | anomaly detection → quarantine | Token-spike / retry-storm / restart-loop signatures → quarantine the agent | parallel | elodin | net-new (~X3) | W2 | med |
| W23 | intake/processing decouple | Threadsafe queue: intake thread(s) enqueue, ONE processing thread drains+ticks; intake never blocks on processing. Design-doc first | 1 Infra (early) | elodin | net-new | — | med |
| W24 | self-driven tick (timerfd/epoll) | Processing tick self-drives via a timerfd/epoll reactor — fixes the RPC-driven-tick fragility every time-based capability leans on | 1 Infra (early) | elodin | net-new (~X4) | W23 | med |

Curated shortlist (§3) in table terms: **W5+W6+W7+W8+W9** (activate causality /
the coordination graph), **W10+W11+W13** (beads-native tracker + auto-routing),
**W17+W18** (dead-man switch + digest).
