# Policy actors — the broker's autonomous-behavior extension point

Status: **design + implementation plan** (elodin, 2026-06-04). This is the
**self-contained plan** for the last run-2 seam cut (Policy), written for
sulin's review. **Gate: sulin approves this doc → extraction commits begin. No
seam-extraction lands before that sign-off.** Owner: elodin. Reference:
[[broker-seam-redesign]] (the seam decomposition this completes).

## 0. Why this seam matters beyond the refactor

The other five seams (Log / Router / Transport / Readers / Daemon) are *done* —
they shrank a 74 KB god-object without adding capability. **Policy is the one
cut that adds a capability**: it is the **extension point every future
autonomous broker behavior plugs into** — the facts-log distiller's clear/notes
triggers, the recovery engine, coordination-pattern hooks (queue/blackboard
nudges). Today each of those would mean *another method bolted onto the Loop*,
re-fusing the god-object. Policy makes them **actors that register against a
fixed interface and never touch the kernel**.

So the deliverable is NOT "move `maybeAutoRecover` to `policy.cpp`." It is an
interface that admits new actors **without reopening the seam**. P2
auto-recovery is the *proving* actor — it fitting cleanly is the evidence the
interface is sufficient, not the goal itself.

### 0.1 The north star — one coordination substrate

The reason to build this well: **the Policy model is the single coordination
substrate for the whole bus.** The same `observe snapshot → guard → emit action`
engine serves *broker-internal autonomy* (recovery, auto-clear, the doorbell)
**and** the *user-facing coordination patterns* (`coordination/`: queue,
blackboard, maildir) that the original design pencilled as a *separate* layer of
per-agent hooks + scripts. Unifying them is the point — and it is a concrete
deliverable improvement, not architecture for its own sake:

- **It answers `coordination/README`'s open question.** That doc leaves "how
  does a pattern activate" *unresolved* ("the shape will emerge when the second
  pattern lands"). Policy is the answer: **the broker-side of a coordination
  pattern IS a Policy actor.** No bespoke loader, no hooks-wiring step.
- **It moves coordination from distributed-and-racy to central-and-durable.** A
  work-queue as per-agent Stop hooks means each agent races to pull the next
  item, only while it's alive, with no shared arbiter. As a Policy actor it runs
  *once, centrally, in the broker*, observing every agent's readiness at once —
  and it **reuses primitives the broker already has**: the queue is an
  append-Log, a claim is a cursor, an assignment is an in-flight record,
  readiness is the snapshot. Most of a coordination pattern is *already built*;
  the actor is the thin part that's missing.
- **One mechanism, one audit trail, one test harness** for recovery and
  coordination alike — instead of two subsystems with two mental models.

This is the direction the build moves toward. But **MVP-first beats any
particular policy**: §5 lands the substrate (proven by recovery) before §6
proves the unification with *one* coordination pattern end-to-end. The bold
extensions (§8) are great-to-have, explicitly non-gating. We earn the vision one
deliverable at a time.

The bet this protects (the broker's thesis): *a durable, out-of-context broker
is a cheaper, more reliable system-of-record than in-context coordination.* Its
irreducible kernel is a **triad** — an append-only byte-offset **Log**, a
per-(topic,consumer) **cursor that advances ONLY on an ack**, and a
**boot-epoch stamp** on every record. Policy actors observe and *enqueue*; they
must never be able to perturb that triad. §2 makes that structural.

---

## 1. The actor interface — the extension contract

### 1.1 What a Policy actor IS

A Policy actor is a **single-threaded `observe → decide → guard → emit-intent`
step the broker loop runs each tick.** Every existing autonomous behavior
(`maybeAutoRecover`, `maybeAutoClear`, the doorbell) already has exactly this
shape; the interface just names it. Lifecycle:

1. **cadence gate** — the actor's own scan rate-limit (recovery: 30 s; doorbell:
   5 s). The loop ticks fast; an actor decides how often it actually runs.
2. **observe** — read a **snapshot value** (§1.3). NO authority: every field is
   a pure derivation reconstructable from `events.jsonl` + an injected pane
   value, so an actor holding a stale read costs nothing.
3. **decide** — a predicate over the snapshot → an intended action.
4. **guard** — the actor's OWN cooldown / backoff / breaker. The recovery ledger
   is the archetype: pure functions over a monotonic clock, persisted by the
   actor, owned by no one else.
5. **emit intent** — return **declarative actions** (§1.4). The loop executes
   them. The actor never advances a cursor, types a pane, or spawns a process
   itself.

### 1.2 The signature

```cpp
// policy.h — namespace bus::policy. A new static lib `bus_policy` that links
// bus_readers + bus_core ONLY (no bus_pane, no rpc/socket). That link boundary
// IS the mechanical guard (§3, §5.4): a Policy actor physically cannot acquire
// a pane or touch the socket — the code won't link.

class PolicyActor {
 public:
  virtual ~PolicyActor() = default;

  // Stable identity — namespaces this actor's audit rows + cooldown keys.
  virtual auto name() const -> std::string_view = 0;

  // The one method. Decide + guard + record-fire, atomically and internally;
  // return the actions for the loop to execute. Pure w.r.t. the broker: no
  // socket, no real time (clocks arrive in ctx), no pane fork beyond
  // ctx.pane(). THIS is the litmus surface — unit-testable with a synthetic
  // context and no broker.
  virtual auto evaluate(const PolicyContext& ctx)
      -> std::vector<PolicyAction> = 0;
};

class PolicyEngine {
 public:
  // The registration surface. Actors register once at broker startup.
  auto registerActor(std::unique_ptr<PolicyActor>) -> void;

  // Fan out over registered actors in registration order, concatenate their
  // emitted actions. Pure: holds only the actor vector, touches no broker
  // resource. The loop executes the returned actions.
  auto evaluate(const PolicyContext& ctx) -> std::vector<PolicyAction>;

 private:
  std::vector<std::unique_ptr<PolicyActor>> actors_;
};
```

### 1.3 What an actor observes — `PolicyContext`

The loop plane builds one context per tick and hands it to every actor. It is a
pure value plus a **lazy pane resolver** — the only escape hatch to expensive
state, kept in the loop plane (where the single-thread TTL cache already lives)
so the cost is paid only by the actor that needs it, only after cheap signals
match (exactly as recovery's R4 forks a pane only after the cheap pre-filter).

```cpp
struct AgentSnapshot {
  std::string name;
  const AgentInfo* info;              // Readers event-fold (turn_start_ms,
                                      //   open_tool, last event, transcript_path…)
  AgentAxes axes;                     // computeAxes WITHOUT pane (cheap)
  std::int64_t transcript_age_ms{-1}; // mtime staleness; -1 = unknown
  bool inbox_pending{false};          // a record sits past the inbox cursor
  bool has_in_flight{false};          // an unacked dispatch for this agent
  bool attached{false};               // hasPresenceFile — human present
  bool blocking_op{false};            // mid /clear or /compact
};

struct PolicyContext {
  std::int64_t now_wall_ms;           // hooks wall-stamp events.jsonl (age math)
  std::int64_t now_mono_ms;           // ledger/guard clock (suspend-immune)
  std::vector<AgentSnapshot> agents;  // live panes only (paneId resolved)
  std::function<const PaneState&(const std::string&)> pane;  // lazy, cached
};
```

**What it deliberately does NOT carry:** the registry, topic logs, the socket,
the in-flight map by reference, or any mutable broker field. An actor sees
*derived reads*, never authority. New observation needs (e.g. the distiller's
token-fill %, already produced into `$STATE/status/<agent>.json`) are added as
**new snapshot fields** — a value the loop plane folds in — never as a handle to
mutable state.

### 1.4 What an actor may do — the action vocabulary

```cpp
struct PolicyAction {
  enum class Kind { Enqueue, Recover, Nudge };
  Kind kind;
  std::string agent;        // target agent (all kinds)
  std::string topic;        // Enqueue: destination topic
  std::string body;         // Enqueue: record body
  std::string protocol;     // Enqueue: SendOpts.protocol (audit / epoch tag)
  int deliver_when{0};      // Enqueue: SendOpts.deliver_when (0=now,1=idle)
};
```

Three primitives, chosen so the common case is generic and the dangerous cases
are named:

- **`Enqueue`** — append a record through the Log into any topic. This is the
  **90 % case** and it is generic over `topic`: `/clear` to `commands-<agent>`,
  a `would-recover` row to `audit`, escalation mail to `inbox-ops`, a
  distiller's `notes-<agent>` post, a coordination pattern's blackboard write —
  all the same primitive. A new actor almost never needs a new kind.
- **`Recover`** — invoke the relaunch primitive (`bus recover <agent>`, bast's
  verb). The heavy, process-level action; named so the audit + the loop see it
  explicitly. (Phase-C of recovery; the vocabulary carries it now so wiring it
  later is a flag flip, not an interface change.)
- **`Nudge`** — a doorbell sentinel submit (`[bus-wake]`) via Transport's TTY
  arm. Wakes an idle agent; named because it actuates a pane.

**The load-bearing omission: there is no `AdvanceCursor` verb, no `WriteCursor`,
no `Ack`.** A Policy actor *cannot* move a cursor, ack a record, or rewrite the
log — the only cursor-advancing path in the broker remains `Loop::onAck`. That
is the kernel triad protected **by construction**: the worst a buggy or
malicious actor can do is enqueue a spurious record (visible in the audit log)
or fire a nudge/recover (guarded by the actor's own breaker). It can never
corrupt delivery state.

### 1.5 Record-without-execute fails safe

An actor records a fire *inside* `evaluate()` (so its backoff arms), then the
loop executes the returned action. If execution fails, the ledger has recorded a
fire that didn't happen → **extra backoff, never an extra action** — the safe
direction. This is exactly today's `recoverClear`: it appends fire-and-forget,
then records. When a heavy action later needs record-on-confirmed-outcome (a
Phase-C relaunch that must not double-fire), add an optional
`onResult(action, ok)` callback to `PolicyActor` — a documented future hook,
explicitly NOT built now.

---

## 2. Why this is a real extension point, not P2-shaped

The test of a genuine extension point: future actors that look *nothing like*
recovery drop in without touching Log / Router / Readers or adding an action
kind. Three concrete walkthroughs:

- **facts-log distiller trigger (sulin's P3).** A `DistillerActor::evaluate()`
  reads the same snapshot — `axes`, `info.last.event`, plus a token-fill
  snapshot field — and on a subtask-boundary or fill-% threshold returns
  `Enqueue{notes-<agent>, …}` or `Enqueue{commands-<agent>, "/clear"}`. Its own
  cooldown lives in the actor. **No new kind, no kernel touch, no snapshot
  authority** — it consumes a value the loop already folds in.
- **a coordination pattern (queue / blackboard).** An actor watching a
  work-queue topic's depth returns `Enqueue{<pattern-topic>, …}` to rebalance.
  Generic `Enqueue` is the entire mechanism — "watch state, post a record" is
  what most coordination patterns *are*.
- **recovery's own heavy rungs (Phase C).** `Recover` + `Nudge` are already in
  the vocabulary; enabling them is flipping the `RecoveryActor` mode flag, not
  changing the interface.

If a future actor ever needs a verb that isn't `Enqueue`/`Recover`/`Nudge`,
*that* is the signal to revisit — and adding a kind is a localized vocabulary
change, never a re-cut of the kernel seams.

### 2.1 Composition — the engine never arbitrates

§2 proves each actor in isolation, but Policy *exists* for the multi-actor
future, so the contract must answer: **two actors target the same agent in one
tick — what arbitrates?** Answer: **nothing in the engine.** `PolicyEngine`
concatenates emitted actions in **registration order** (deterministic — a mild,
free ordering guarantee, not a priority system) and the loop executes them.
There is no priority, no preemption, no cross-actor conflict detection in the
engine — and there must not be, because that would re-introduce the cross-actor
coupling the §4 leaf-guard forbids.

**Conflicts resolve at execution, through the kernel's existing per-mechanism
gating — the exact gating that already tolerates a human *and* the broker
enqueuing to the same agent today:**

- **`Enqueue` to a `commands-<agent>` / inbox topic:** the in-flight gate
  serializes — `dispatchTuiCommands` dispatches **≤ 1 command per agent at a
  time** (`if (in_flight_.contains(m.id)) return;`), and `/clear` + `/compact`
  additionally set a **blocking-op** that defers all further delivery to that
  agent until the Stop event acks it. So two actors both enqueuing `/clear` in
  one tick → two records, **serialized**: the first clears the agent; the second
  delivers next tick as a benign redundant clear of a just-cleared (idle) agent.
  Redundant, never corrupting.
- **`Recover`:** the `RecoveryActor` breaker (MaxR/MaxT) + the per-agent recovery
  epoch dedup a double respawn. `Recover` is single-owner by design (the
  recovery actor); there is no second emitter to race.
- **`Nudge`:** the doorbell per-agent cooldown coalesces repeated wakes.

**Why this is design-complete, not a punt.** Every action in the vocabulary is
**idempotent or cheaply-redundant** at execution (a clear, a nudge, an audit
append, a notes post). For such actions, serialized execution-gating *is* full
conflict resolution — and it is gating the kernel already owns. An engine-level
arbiter would duplicate that gating and couple actors together to do it.

**The precise revisit-signal:** a future pair of actors with *non-idempotent,
mutually-conflicting* intents on one agent — where serializing both is *wrong*,
not merely redundant. That is the same class of signal as "an actor needs a verb
outside `Enqueue`/`Recover`/`Nudge`": a conscious, localized decision made *then*
(add a coalescing key or a priority field to the vocabulary), not arbitration
machinery built speculatively now. Naming the trigger here is the decision; we
do **not** defer an unmade choice to actor #2.

---

## 3. P2 as the proving actor — concrete mapping, zero behavior change

`RecoveryActor : PolicyActor` reconstructs `maybeAutoRecover` exactly. The
mapping is mechanical:

| Today (loose on `Loop`, `delivery.cpp`) | After (owned by `RecoveryActor`) |
|---|---|
| `recovery_` (per-agent `RecoveryLedger`) | actor member |
| `would_recover_next_log_ms_` (per-sig cooldown) | actor member |
| `auto_recover_last_scan_ms_` (30 s cadence) | actor member — the §1.1 cadence gate |
| `last_tick_wall_ms_ / _mono_ms_ / suspend_grace_until_mono_ms_` | actor members (the wall-jump grace) |
| `maybeAutoRecover()` body (R1–R4 signatures) | `RecoveryActor::evaluate(ctx)` |
| `recoveryDecide` / `recoveryRecord` / `recoveryObserveHealthy` (already pure, `recovery.h`) | called from `evaluate()` — unchanged |
| `recoverClear` lambda: enqueue `/clear` to `commands-<agent>` + audit | returns `Enqueue{commands-<agent>,"/clear",protocol="auto-recover-clear",deliver_when=1}` + `Enqueue{audit,…,protocol="recover"}` |
| `logWould` lambda: append `would-recover` audit row | returns `Enqueue{audit,…,protocol="would-recover"}` |
| `loadRecovery` / `saveRecovery` (I/O) | stays in the loop plane; the actor calls back through a small persistence hook, OR the loop loads/saves around `evaluate()` — I/O is loop-plane, exactly as the seam doc keeps pane-acq there |

What the loop plane does each tick (replacing the inline `maybeAutoRecover()`
call in `tick()`):

```
runPolicy():
  ctx = build PolicyContext   // readAgents + computeAxes + transcript stat +
                              //   inboxPending + has_in_flight + presence +
                              //   blocking_op  — the exact prologue
                              //   maybeAutoRecover already computes
  actions = engine_.evaluate(ctx)
  for a in actions:           // execute via existing paths
    Enqueue → registry get/create topic + TopicLog::append(stampEpoch, opts)
    Recover → bus recover <agent>      (Phase C; inert until then)
    Nudge   → doorbell sentinel submit (Transport TTY arm)
```

**Zero behavior change**, and the safety margin is large: the default mode is
`observe`, in which the only emitted actions are `would-recover` audit rows —
so even a bug in the extraction can at worst mis-log an audit line, never act on
an agent. The deploy-verify (§5.5) confirms the `would-recover` stream is
byte-identical before/after.

**The observe/soft/on flag stays orthogonal.** `CLAUDE_BUS_AUTO_RECOVERY` is a
`RecoveryActor` constructor argument — a *runtime* decision about whether emitted
actions are real or logged-only. The `PolicyEngine` never sees modes; the
interface has no concept of one. (`observe` → emit `would-recover` Enqueues
only; `soft` → also emit the R1 `/clear` Enqueue; `on` → also emit `Recover`.)
This is a property of one actor, not the seam.

---

## 4. The §6 leaf guard — no shared mutable field across actors

Policy is a DAG leaf (`dependsOn` Router + Readers). The relabel test for this
seam: **if any actor needs mutable state another actor also mutates, the seam
has failed** — that shared field would re-fuse the god-object inside the Policy
plane. The discipline:

- Each actor owns its own state (`RecoveryActor` owns `recovery_`); the
  `PolicyEngine` holds *only* the actor vector — no shared scratch.
- Cross-actor communication, when it's ever needed, goes through the **Log**
  (one actor enqueues, another observes the topic via the snapshot) or through a
  **new read-only snapshot field** — never a shared C++ member. That keeps every
  interaction on the durable, auditable substrate instead of in hidden RAM.

This is enforceable mechanically (§5.4): the `bus_policy` lib links only
`bus_readers` + `bus_core`, so an actor cannot reach the pane cache or the
socket; a shared mutable broker field can't be referenced because it isn't in
the link graph.

---

## 5. Build sequence — incremental, broker-live at every commit

Each commit leaves the broker delivering mail. The ordered list (auri's
sequencing, task #…-142d):

### 5.1 Commit 1 — `bus::policy` lib + `RecoveryActor` + engine wired *(PURE EXTRACTION — zero behavior change; the deploy-verified anchor)*

The whole seam in one coherent, behavior-preserving commit:

- Add `src/policy.h` + `src/policy.cpp` and a `bus_policy` static lib
  (`bus_readers` + `bus_core` only — §5.4). Define `PolicyAction`,
  `AgentSnapshot`, `PolicyContext`, `PolicyActor`, `PolicyEngine`.
- Add `RecoveryActor : PolicyActor` = `maybeAutoRecover`'s body **verbatim**
  (§3), owning `recovery_` + the cooldown/grace members moved off `Loop`.
- Wire it: the loop registers the actor at startup, builds `PolicyContext` each
  tick, and `tick()`'s `maybeAutoRecover();` becomes `runPolicy()` (build ctx →
  `engine_.evaluate(ctx)` → execute returned actions via the existing
  append/recover/doorbell paths). Delete the old method.

The execution path inverts from *inline-act* to *return-then-execute*, but the
emitted records, topics, epochs, cadence, and guards are unchanged. **No
behavior change.** This is the seam *and* its proof in one landing — no
dead-interface pre-commit (a lib with no actor is noise; landing the reference
actor with it makes the commit meaningful and broker-live).

Verify:
- **observe audit stream identical** — seed/observe the live `would-recover`
  stream before vs after; byte-identical is the no-behavior-change proof.
- **`RecoveryActor::evaluate` unit test** — synthetic context (a wedged + a
  healthy agent) → `would-recover` actions for the wedged one, none for the
  healthy one, ledger advances on an R1 clear (the observe-itest lifted to the
  actor level — the litmus).
- **`PolicyEngine` fan-out unit test** — a fake actor returns canned actions;
  assert registration-order concatenation (§2.1).
- existing `dup-delivery-itest`, `off-tty-itest`, `recover-itest`, P2
  observe-itest: all green.

*Blast-radius note:* default mode is `observe`, so even a bug here can at worst
mis-log an audit row — there is no path to acting on an agent. That is why the
extraction + wire is safe as one commit rather than a shadow-mode pair.

### 5.2 Commit 2 — `maybeAutoClear` → `AutoClearActor` *(LATER; behavior-reviewed)*

`maybeAutoClear` and recovery's R1 coexist today (observe: auto-clear acts,
recovery logs; soft: R1 supersedes). Folding auto-clear into a *second* actor
proves the multi-actor registration path (§2.1) but is **not required for the
seam proof** and changes the coexistence — so it lands separately, after sign-off,
as its own behavior-reviewed commit (or is retired once R1-soft is default). Not
bundled into Commit 1.

### 5.3 Commit 3 — wire `Recover` / `Nudge` heavy rungs *(PHASE C; gated)*

The `Recover` + `Nudge` action kinds exist in the vocabulary from Commit 1 but
are inert until wired. Enabling them is recovery's Phase C — gated on the
observe→soft→on activation sequence ([[project_p2_auto_recovery]]) and bast's
`bus recover` contract, **not** part of the seam refactor. A flag flip + an
executor branch, not an interface change.

### 5.4 Where the mechanical guard lands

The Readers-style enforcement is the **link boundary**: `bus_policy` links
`{bus_readers, bus_core}` and nothing else. CMake + an `nm`-symbol check (same
technique as the Readers extraction, commit `eba57df0`) assert `policy.o`
references no pane-acquisition or socket symbols. An actor that tries to fork a
pane or grab a shared mutable broker field **won't link** — the §4 leaf guard
made structural, not a code-review convention.

### 5.5 Regression + deploy-verify (every commit)

- Unit + itest suite green (`ctest`): the new policy units plus the full
  delivery/recover/off-tty itests.
- Deploy-verify: canonical settle → SIGTERM the broker (it carries the fix) →
  floating relaunch (`zellij action new-pane --floating -- …/bus broker run`,
  never `nohup`) → `bus broker info` shows the new `build_commit` → a delivery
  smoke (mail round-trips) → the `would-recover` audit stream matches the
  pre-extraction baseline.
- Isolated `CLAUDE_BUS_STATE` for any risky step before the live broker.

### 5.6 Kernel-triad survival (explicit)

- **append-log:** `Enqueue` only ever appends through the Log; no action rewrites
  or truncates it.
- **cursor-advances-on-ack-only:** the action vocabulary has no cursor verb;
  `Loop::onAck` remains the sole advance path, untouched by this seam.
- **boot-epoch:** every `Enqueue` is stamped with `current_epoch_` exactly as
  `recoverClear` stamps it today, so epoch-fenced quarantine still holds.

---

## 6. The unification — coordination patterns are Policy actors

This is the *north star* (§0.1) made into a build milestone. After §5 proves the
substrate with recovery, the next deliverable proves the substrate is **general**
— that a coordination pattern, which looks nothing like recovery, drops onto the
same interface and needs no separate hooks-and-scripts layer.

**The reframe.** `coordination/`'s patterns are today specified as per-agent
plumbing (an agent-prompt teaching idle behavior + a Stop hook that pulls the
next queue item + CLI helpers). Re-expressed as Policy actors, the *broker-side*
of a pattern becomes one central actor:

| Coordination pattern | As a Policy actor | Reuses (already built) |
|---|---|---|
| **work-queue** — agents pull tasks when idle | `QueueActor`: watch queue depth × ready agents → `Enqueue` an assignment to the next idle agent | queue = append-Log; claim = cursor; assignment = in-flight; readiness = the snapshot |
| **blackboard** — shared notes all read | `BlackboardActor`: on a new post to the board topic → `Enqueue` a notify to subscribers | board = a topic; fan-out = Enqueue per subscriber |
| **maildir** — async per-agent mailboxes | *already the agent-inbox topic + delivery loop* — the pattern mostly exists | inbox topic + cursor + the delivery kernel |

The point of the table is not the specific patterns — it is that **the broker
already owns most of every cell's right column.** A coordination pattern is the
thin policy actor on top of primitives the kernel already provides.

### 6.1 The unification MVP (the milestone, MVP-first)

**Deliverable:** *one* coordination pattern, end-to-end, as a single Policy
actor — proving "coordination = policy" with running code, not a doc claim.

- **Pick the leanest, not the favourite.** The work-queue is the natural first
  *because the broker primitives already exist* (append-Log + cursor + in-flight
  + readiness) — so the new code is just the dispatch actor, the smallest
  possible proof. But the milestone is the *framework result* — "a non-recovery,
  coordination-shaped actor runs on the same engine" — not the queue's feature
  set. Resist gold-plating the chosen pattern (§0.1: MVP over any particular
  policy).
- **What it proves:** a pattern activates by *registering an actor*, with **no
  bespoke loader and no per-agent hooks** — closing `coordination/README`'s open
  "how does a pattern activate" question.
- **Gating:** this is the milestone *after* the seam lands (§5 C1). It does NOT
  gate C1, and it is its own design-doc + sulin-review pass — same discipline as
  the seam. Named here so the build has a north star, scoped here so it stays
  MVP.

---

## 7. The litmus, restated

A seam is real iff it's testable without a broker. `RecoveryActor::evaluate(ctx)`
takes a hand-built `PolicyContext` (synthetic `AgentSnapshot`s, a fake `pane`
resolver, injected monotonic clocks) and returns `PolicyAction`s you assert on —
no socket, no real pane, no real time. It is the same shape as the existing
`recoveryDecide`/`recoveryRecord` unit tests, lifted to the actor level. That
the *proving* actor passes the litmus, and that three unrelated future actors
(§2) drop onto the same interface, is the evidence the cut is structural and not
a relabel.

---

## 8. Gate + sequence

1. **This doc → auri review → sulin approval.** No seam-extraction commits until
   sulin signs off.
2. **Commit 1** (§5.1) — `bus::policy` lib + `RecoveryActor` verbatim + engine
   wired; zero behavior change; deploy-verified vs the live observe audit stream.
   This is the seam — it lands the substrate.
3. **Unification MVP** (§6.1, its own design+review pass) — *one* coordination
   pattern (lean first: the work-queue) as a Policy actor, proving the substrate
   is general. The north star; does not gate Commit 1.
4. **Commit 2** (§5.2, later) — `AutoClearActor` fold — and **Commit 3** (§5.3,
   Phase C, gated) — wire `Recover`/`Nudge` — are *separate*, behavior-reviewed
   steps downstream of the P2 activation sequence
   ([[project_p2_auto_recovery]]), not the seam refactor.

## 9. Future directions (non-gating, great-to-have)

The substrate already admits these — none needs a kernel change; each is a new
actor (or a small engine affordance), landed when it earns priority. Listed so
the design space is on record, *not* as committed scope.

- **Coordination breadth.** Once the §6 MVP proves the shape, the remaining
  `coordination/` patterns (blackboard, richer queues, maildir niceties) land as
  further actors — and the originally-planned hooks+scripts coordination layer is
  *retired in favour of actors*. One subsystem, not two.
- **A declarative policy DSL.** Most policies reduce to `(predicate over
  snapshot) → Enqueue(topic, body)`. Express them as *data* on a
  `policy-registry` topic the engine hot-loads — add a coordination rule with no
  recompile. The compiled actors stay the privileged core; the DSL covers the
  long tail.
- **A self-tuning meta-actor.** Recovery thresholds (backoff, MaxR/MaxT,
  idle-clear-min) are human-guessed env knobs. A `MetaActor` observes the audit
  log of *would-recover vs. actual outcome* and rewrites other actors'
  parameters — the breaker's false-positive rate becomes a gradient it descends.
  This is "policy is a theory that UPDATES" ([[project_p3_context_watchdog]])
  made structural.
- **Adversarial actors.** Borrow the workflow adversarial-verify pattern, live:
  a proposer `Enqueue`s to a `proposed-actions` topic; a skeptic actor must
  *refute* before a destructive action (clear/relaunch) executes. The
  engine's no-arbitration rule (§2.1) is what *enables* this — the debate isn't
  engine machinery, it's just more actors talking through the Log.

The through-line: **once actors are kernel-safe (§1.4) and converse through the
durable Log, the broker stops being a delivery mechanism and becomes a
programmable, self-observing control plane.** Boldness is cheap here precisely
because the kernel triad is fenced off by construction — the worst a runaway
actor can do is emit an auditable record.

See [[project_broker_seam_redesign]] for the full six-seam decomposition this
completes; this doc is the durable contract future actor-authors read.
