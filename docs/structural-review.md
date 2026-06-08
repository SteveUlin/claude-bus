# Structural Review — claude-bus C++23

A living proposal list. Walk through it with Claude by referencing item numbers (e.g. "do #C3"). Each item has a status checkbox; check it off when landed.

**Purpose:** harden the API shape and module boundaries so a future less-capable model can extend the codebase without misusing it. Every proposal is a structural improvement — not a bug fix, not a style nit.

> A completeness-critic pass (see **Completeness-critic addenda** before the sequence section) folded in 7 gaps the first pass missed and flagged 3 items (B2, C4, F3) as exceeding the surgical bar — read those before scheduling those three.

## Landed (autonomous drive, 2026-06-07)

Driven as separate build-green, force-verified commits on `main`. Skip these in the walk-through:

- **J1** — `CursorStore` extraction; `Journal` is now total + path-only (`1668be4c`)
- **C1** — `TopicKind` enum replaces the raw `kind` string, 39 sites (`0194b202`)
- **D1** — `TokenWatcher` extracted out of the `delivery::Loop` god-object (`1b334a55`)
- **D3 + D4** — `emitAudit()` + `isAgentIdle()` private helpers in `delivery::Loop` (`fcf9f081`)

Also landed earlier in the session (journal kernel): in-place trim removed (offsets immutable), `retention_ms` dropped, read-truncation surfaced (`truncated_at`), per-file UUID header (wire v7, tag from UUID).

**Deferred deliberately:** **J2** (open/create factories — changes absent-file semantics + needs a Result-factory ergonomics decision) and **D2** (epoch helpers → `bus::msg` — lower value than the doc claims: broker keeps `delivery.h` for `Loop`, so it doesn't decouple). Both want a quick decision in the walk-through.

---

## Principles applied

1. **Partial objects** — a type whose method validity depends on *how* it was constructed is a broken interface. Split into total types.
2. **Leaky boundaries** — higher-level metadata must not leak down; internal state (paths, offsets, impl structs) must not leak up.
3. **Free / static / member** — free functions are fine; no bias against them. The rule is narrower: when a helper is for *one particular class*, prefer a `static` member on that class over a loose free function (it namespaces the association). Keep each class's public *member* (instance) API minimal — instance methods only for what genuinely needs the object's state + invariant. Generic helpers (not tied to one class) stay free functions.
4. **Redundant data** — derive over store unless caching is a measured win.
5. **Crash vs Result** — programmer errors (precondition violations, bad calls) → `fatal()`, always-on. Operational failures (I/O, parse) → `Result<T>`. Never NDEBUG-gated precondition checks, never silent no-ops on bad calls.
6. **Minimal surface** — no public surface that has no external callers; no forgeable raw strings where an opaque type belongs; missing predicates that force callers to re-spell literals.
7. **Cohesion / coupling** — god-objects split by responsibility; modules don't reach into each other's internals.
8. **Naming** — a low-level type named for a high-level use couples the name to one consumer.
9. **Types** — text → `std::string`; opaque handles via private-ctor + friend-factory; enums over raw strings for bounded domains.

---

## Cross-cutting proposals

These span multiple modules; they unlock or simplify several per-area items below.

---

### C1 — TopicKind enum replaces raw `kind` string throughout

**Principle:** 9 (Types), 6 (Minimal surface)
**Severity:** HIGH

**Current:** `TopicConfig::kind` is `std::string`. All kind-checks use `cfg.kind == std::string{kKindAgentInbox}` — a heap allocation per comparison. 39 raw `kKind*` comparisons scatter across `broker.cpp` (9), `delivery.cpp` (7), `sub_topic.cpp`, and `topic_registry.cpp`. The `== std::string{kKind...}` idiom is both slow and forgeable.

**Proposed:** Replace `std::string kind` with `enum class TopicKind { AgentInbox, TuiCommands, PubSub, WorkQueue, Blackboard, AppendLog, Unknown }` in `topic_registry.h`. Add `topicKindFromStr(std::string_view) → TopicKind` and `topicKindToStr(TopicKind) → std::string_view` as the single serialization bridge in `topic_registry.cpp`. Delivery's if/else-if chains become exhaustive `switch` statements the compiler checks. Kind predicates (finding B4 below) become one-line `return kind == TopicKind::AgentInbox`.

**Blast radius:** 39 comparison/assignment sites in `broker.cpp`, `delivery.cpp`, `sub_topic.cpp`, `topic_registry.cpp`. Serialization/deserialization in `topic_registry.cpp` is the one non-mechanical site. Medium risk, single-cluster.

**Status:** [ ] proposed

---

### C2 — `ansi.h` + `time.h`: extract terminal constants and `nowMs()` out of `agent_status.h`

**Principle:** 7 (Cohesion), 2 (Leaky boundaries)
**Severity:** HIGH

**Current:** `agent_status.h` bundles three unrelated concerns: (1) agent status computation (`AgentInfo`, `AgentAxes`, `computeAxes`, `readAgents`), (2) 16 ANSI escape constants in `bus::ansi`, (3) `nowMs()` / `systemBootMs()` time utilities. Six viewer TUs (`sub_monitor.cpp`, `sub_agent_bar.cpp`, `sub_log.cpp`, `sub_tasks.cpp`, `sub_triggers.cpp`, `sub_verify.cpp`) include the full agent-status compilation unit solely for `using namespace bus::ansi`. Four TUs (`task_model.cpp`, `sub_produce.cpp`, `sub_tasks.cpp`, `sub_triggers.cpp`) pull in all of agent-status solely for `nowMs()`. `journal.cpp` carries a private anonymous-namespace copy of `nowMs()` — an inconsistency. Every include of `agent_status.h` for a trivial utility pays the full compilation cost of `readAgents`, `computeAxes`, and all their dependencies.

**Proposed:** Create `src/ansi.h` with the `bus::ansi` namespace. Create `src/time_util.h` (or include `nowMs()` in `ansi.h` — it's also 3 lines) with `bus::nowMs()` and `bus::systemBootMs()`. `agent_status.h` includes neither; it exposes only status types and functions. The 6 ANSI-only consumers include `ansi.h`; the 4 `nowMs`-only consumers include `time_util.h`. Delete the anonymous copy in `journal.cpp`.

**Blast radius:** 16 `#include "agent_status.h"` sites; ~6 change to `ansi.h`, ~4 gain a lighter include. Zero behavior change. Purely additive + include-path swap.

**Status:** [ ] proposed

---

### C3 — Unify `fatal()` precondition-crash contract; replace all NDEBUG-gated `assert()` on programmer errors

**Principle:** 5 (Crash vs Result)
**Severity:** HIGH

**Current:** `journal.cpp` uses an always-on `fatal()` macro for precondition violations (correct). `envelope.cpp:34–35` uses standard `assert()` for `sender.size() <= 255` and `protocol.size() <= 255` — stripped in NDEBUG builds. `json_min.cpp:300–308` `asArray()`/`asObject()` dereference null `shared_ptr` with no check at all — silent UB/crash with no message. `json_min.cpp:39–45` `asBool()`/`asInt()`/`asString()` silently return zero/empty on a mistyped `Value`. The contract is incoherent: some preconditions abort loudly, some silently corrupt, some vanish in release builds.

**Proposed:** Establish `fatal()` (or `BUS_ASSERT(cond, msg)` that always compiles) as the single precondition mechanism. Add it to all `as*` accessors in `json_min`: `if (type_ != Type::Array) { fatal("json::Value::asArray called on {}", typeStr()); }`. Replace `assert()` in `envelope.cpp` with the same pattern. Document the contract once in `types.h` or a small `assert.h`.

**Blast radius:** 2 lines in `envelope.cpp`, ~6 lines in `json_min.cpp`. The 30 `as*` call sites are unaffected when their prior guard is correct; broken sites get caught at the call site in any build mode.

**Status:** [ ] proposed

---

### C4 — `StatePaths` value type: consolidate stateRoot sub-path construction

**Principle:** 3 (Free vs method), 2 (Leaky boundaries)
**Severity:** MEDIUM

**Current:** `stateRoot()` returns `std::string`. 23 call sites across ~12 source files append raw literal suffixes: `stateRoot() + "/broker.sock"`, `stateRoot() + "/topics/" + name + ".log"`, `stateRoot() + "/done"`, `stateRoot() + "/presence/" + name`, `stateRoot() + "/status/" + agent + ".json"`, etc. Each well-known sub-path is re-spelled at the call site — a misspelling is a silent runtime miss, not a compile error. `sub_roles.cpp:42,45,47` hardcodes `/home/sulin/claude-bus/roles` as a fallback — a machine-specific path in a tracked public-repo source file.

**Proposed:** Augment `state_paths.h` with named accessor functions (or a thin `StatePaths` struct built from `stateRoot()`): `brokerSock()`, `topicLog(name)`, `donePath(agent)`, `presencePath(agent)`, `statusPath(agent)`, `dynamicPeersPath()`, `cursorsDir(topic)`, etc. `stateRoot()` stays for the broker config struct. Fix `sub_roles.cpp` fallback to use the executable-relative path or return an error — not a hardcoded home directory.

**Blast radius:** 23 call sites across ~12 files — all mechanical. Additive: introduce named accessors, migrate sites, keep `stateRoot()`. The `sub_roles.cpp` fallback fix is 3 lines.

**Status:** [ ] proposed

---

### C5 — Raw string enums unified: Task::state, OwnerLive::boundary, RecoveryMode — introduce typed enums

**Principle:** 9 (Types), 6 (Minimal surface)
**Severity:** MEDIUM

**Current:** Three separate raw-string scalar fields encode bounded domains:
- `Task::state` (`"done" | "open" | "in_flight" | "cancelled" | "unknown"`) — 13 comparison/assignment sites across `task_model.cpp` and `sub_tasks.cpp`.
- `OwnerLive::boundary` (`"done" | "idle" | "none"`) — populated from a `Boundary` enum that already exists in `trigger_feed.h`; the roundtrip re-parses a known-good enum back to a raw string.
- `RecoveryMode` (`"off" | "soft" | "on" | "0"`) — stored as `std::string mode_` in `RecoveryActor`, read from env and compared again independently in `delivery.cpp::maybeAutoClear`.

**Proposed:**
- `Task::state` → `enum class TaskState { Open, InFlight, Done, Cancelled, Unknown }` in `task_model.h`. `taskStateStr()` for display/serialization.
- `OwnerLive::boundary` → `Boundary` directly. Add `stringToBoundary(string_view)` as inverse of `boundaryStr()` in `trigger_feed.cpp`.
- `RecoveryMode` → `enum class RecoveryMode { Off, Observe, Soft, On }` in `recovery.h`. Add `recoveryModeFromEnv()` factory that `fatal()`s on unknown values. Both `RecoveryActor` and `delivery.cpp` call the factory once.

**Blast radius:** ~13 sites for TaskState, 3 for Boundary, 5 for RecoveryMode. All mechanical; no logic change. Serial work across three files.

**Status:** [ ] proposed

---

### C6 — `PaneId` strong type: distinguish pane ids from agent names at the type level

**Principle:** 9 (Types), 6 (hard-to-misuse)
**Severity:** MEDIUM

**Current:** `paneId()` returns `std::string` containing `"terminal_N"`. `sendToPane` and `sendKey` take `std::string_view pane_id`. `paneState` and `sendToPaneSafe` take `std::string_view name` (agent name). The two conceptual id types are indistinguishable to the compiler; passing an agent name to `sendToPane` is a silent runtime miss. `dispatch.cpp:87` and `delivery.cpp:106` hold both in local variables named `pane` and `agent` — one transposition away from misfiring the wrong zellij target.

**Proposed:** `struct PaneId { std::string value; }` (or a private-ctor opaque type) returned by `paneId()` and consumed by `sendToPane`/`sendKey`. The compiler rejects a bare `std::string` or agent name at the actuator call sites.

**Blast radius:** `paneId()` has ~8 call sites; `sendToPane` has 4; `sendKey` has 5. All mechanical wrapping changes.

**Status:** [ ] proposed

---

## Per-subsystem proposals

---

### Journal / Envelope (journal-pending)

#### J1 — Extract `CursorStore`: split the partial Journal object into two total types

**Principle:** 1 (Partial objects)
**Severity:** HIGH

**Current:** `Journal` has two construction paths: `Journal(path)` and `Journal(state_root, name)`. The three cursor-store methods — `consumerCursor`, `ack`, `lastAckedId` — call `fatal()` on the path-only form. `state_root_` and `name_` are dead weight on every path-only instance. Private helpers `cursorFilePath` and `lastIdFilePath` compound this — they exist solely to serve the cursor methods. 17 `.consumerCursor` + 12 `.ack` + 1 `.lastAckedId` call sites in `delivery.cpp` and `broker.cpp` implicitly assume they hold the two-arg form.

**Proposed:** Extract `CursorStore(state_root, name)` as a separate total type. Every `CursorStore` method is always valid. `Journal` becomes total and path-only; `state_root_`, `name_`, `cursorFilePath`, `lastIdFilePath` are deleted. Callers that currently hold a two-arg `Journal` hold a `(Journal, CursorStore)` pair instead — both are cheap to construct together. `Journal::tagOf(path)` (or a narrow tag accessor) enables the cross-journal crash guard in `CursorStore`.

**Blast radius:** 30 cursor-method call sites in `delivery.cpp` and `broker.cpp`. Each site already has both the path and (state_root, name), so the mechanical change is straightforward. Prerequisite for broker.cpp log cache consolidation (B1 below).

**Status:** [ ] proposed

---

#### J2 — Journal::open / Journal::create factories: eliminate silent lazy creation

**Principle:** 2 (Leaky boundaries), 5 (Crash vs Result)
**Severity:** MEDIUM

**Current:** `Journal::append` silently creates the on-disk file on first write (`ensureHeader` via `O_CREAT|O_EXCL`). A reader that constructs `Journal(path)` on a non-existent path gets empty results from `peek`/`dump` with no diagnostic — a silent miss that looks like an empty log.

**Proposed:** Static factories: `Journal::open(path)` asserts the file exists (operational error if absent); `Journal::create(path)` writes the v7+UUID header and fails if the file is already present; `Journal::openOrCreate(path)` composes the two. Existing callers name their intent explicitly. The lazy path in `append` is removed.

**Blast radius:** ~30 `Journal(path)` construction sites — all need a one-line mechanical change. The `ensureHeader` logic moves into `create`.

**Status:** [ ] proposed

---

#### J3 — Delete `Journal::cursorToToken` / `Journal::cursorFromToken` wrapper methods

**Principle:** 6 (Minimal surface)
**Severity:** LOW

**Current:** `Cursor::toToken()` and `Cursor::fromToken()` are already public on `Cursor`. `Journal::cursorToToken(c)` and `Journal::cursorFromToken(token)` are one-line forwarders adding nothing. 9 external call sites use the `Journal::` prefix solely because the wrappers existed first.

**Proposed:** Delete both wrappers. 9 call sites in `delivery.cpp` and `broker.cpp` switch to `c.toToken()` / `Cursor::fromToken(token)`.

**Blast radius:** 9 sites — mechanical rename.

**Status:** [ ] proposed

---

#### J4 — Replace `assert()` with `fatal()` in `envelope.cpp` (see also C3)

**Principle:** 5 (Crash vs Result)
**Severity:** LOW

**Current:** `encodeEnvelope` (line 34–35) asserts `sender.size() <= 255` and `protocol.size() <= 255` with standard `assert()` — stripped in NDEBUG builds.

**Proposed:** Replace both with `fatal()` (same pattern `journal.cpp` uses). This is a specific instance of the cross-cutting C3 fix.

**Blast radius:** 2 lines in `envelope.cpp`.

**Status:** [ ] proposed (subsumed by C3)

---

### Broker RPC (broker-rpc)

#### B1 — Promote `getOrOpenLog` cache to store two-arg `Journal` (or `CursorStore` pair) — eliminate ephemeral cursor re-opens

**Principle:** 1 (Partial objects), 4 (Redundant data)
**Severity:** HIGH — prerequisite: J1 (CursorStore split)

**Current:** `getOrOpenLog(name)` returns a path-only `Journal` (cached). Every cursor operation opens a second `Journal{cfg.state_dir, name}` ephemerally — same file, two handles, cursor files re-read. The `fetch` handler opens three instances: `cursor_log` (consumerCursor), `getOrOpenLog` (peek), `fetch_log` (consumerCursor again + ack). The `cursor_log.consumerCursor` result is discarded and re-read by `fetch_log`.

**Proposed:** Once J1 lands: cache a `(Journal, CursorStore)` pair in `getOrOpenLog`. All cursor-method calls use the cached `CursorStore`. The three-instance `fetch` handler collapses to one pair. Until J1: promote `getOrOpenLog` to store `Journal{cfg.state_dir + "/topics", name}` (two-arg form) so cursor reads share the cached instance.

**Blast radius:** 6 sites in `broker.cpp` (enqueue blackboard, peek, state unread-count, fetch ×3, drain, drop). No caller API change.

**Status:** [ ] proposed

---

#### B2 — Break `runBroker` into handler groups registered on a `BrokerState` struct

**Principle:** 7 (Cohesion)
**Severity:** HIGH

**Current:** `runBroker` is a 950-line single function (lines 211–1163 of `broker.cpp`). All lifecycle (singleton guard, epoch management, wipe-on-boot), all 14 RPC handler registrations (ping, stop, info, topic_*, enqueue, peek, state, body, fetch, drain, drop, inflight, gc), and all shared state (registry, logs cache, delivery loop) live in one closure. `recordToJson` and `getOrOpenLog` are closure-local helpers with no reuse surface.

**Proposed:** `BrokerState` struct holding shared mutable state (registry, logs cache, current_epoch, delivery `Loop` reference). Extract handler groups: `registerLifecycleHandlers`, `registerTopicHandlers`, `registerMessageHandlers`, `registerDeliveryHandlers`, `registerGcHandlers` — each accepts `BrokerState&` and registers its subset on `Server`. `recordToJson` and `getOrOpenLog` become members or helpers on `BrokerState`. `runBroker` becomes the orchestrator: lifecycle + wiring only.

**Blast radius:** Self-contained to `broker.cpp`. Public API (`runBroker`, `resolveConfig`, `BrokerConfig`) unchanged.

**Status:** [ ] proposed

---

#### B3 — `dispatch.h` signature: drop `BrokerConfig`, pass `state_dir` directly

**Principle:** 2 (Leaky boundaries)
**Severity:** MEDIUM

**Current:** `dispatchTui(const BrokerConfig& cfg, ...)` includes `broker.h` to get `BrokerConfig`. Inside `dispatch.cpp` the only field accessed is `cfg.state_dir`. `delivery.cpp` also passes the full `BrokerConfig` when only `state_dir` is needed. The TUI dispatch layer is coupled to the broker process structure for no reason.

**Proposed:** Change signature to `dispatchTui(std::string_view state_dir, std::string_view agent, std::string_view body)`. Remove `broker.h` from `dispatch.h`. Callers pass `cfg.state_dir` directly.

**Blast radius:** 2 call sites in `delivery.cpp`, 1 definition in `dispatch.cpp`/`dispatch.h`. Trivial.

**Status:** [ ] proposed

---

#### B4 — Kind-predicate methods on `TopicConfig` (see C1 for the enum upgrade)

**Principle:** 6 (Minimal surface)
**Severity:** MEDIUM

**Current:** 16 raw `tcfg->kind == std::string{kKindAgentInbox}` comparisons in `broker.cpp` (9) and `delivery.cpp` (7), each constructing a heap `std::string` per call.

**Proposed:** Add predicate methods: `isAgentInbox()`, `isBlackboard()`, `isPubsub()`, `isTuiCommands()`, `isWorkQueue()`, `isAppendLog()`, `isSingleRecipient()` (combined AgentInbox||TuiCommands). After C1, each predicate is one line against the enum. Before C1, they still centralize the literal strings.

**Blast radius:** 16 sites in `broker.cpp` + `delivery.cpp`. Mechanical; zero semantic change. Can be done independently of C1 or as its implementation step.

**Status:** [ ] proposed

---

#### B5 — GC handler: `Journal::cursorsDir()` static — stop encoding Journal's internal layout in broker

**Principle:** 2 (Leaky boundaries)
**Severity:** MEDIUM

**Current:** GC handler (`broker.cpp:1123`) removes cursor files with `fs::remove_all(cfg.state_dir + "/cursors/" + tc.name, ec)` — hardcoding the layout that `Journal::cursorPathImpl` encodes internally. A Journal storage-layout change breaks broker silently.

**Proposed:** Add `static Journal::cursorsDir(std::string_view state_root, std::string_view name) → std::string`. GC calls it instead.

**Blast radius:** 1 call site in `broker.cpp`. Add one static method to `Journal`.

**Status:** [ ] proposed

---

#### B6 — `TopicConfig::makeAudit()` factory: eliminate 9 hand-rolled audit-config constructions

**Principle:** 6 (Minimal surface), 3 (Free vs method)
**Severity:** LOW

**Current:** 9 sites in `broker.cpp` and `delivery.cpp` hand-construct an audit `TopicConfig` with `audit.name = "audit"; audit.kind = std::string{kKindAppendLog};` before calling `registry_.create(audit)`. Post-C1, the factory is also the right place to set `parsed_config = AppendLogConfig{}` so the kind↔parsed_config invariant is upheld by construction.

**Proposed:** `static TopicConfig TopicConfig::makeAudit()` (or `makeAppendLog(name)`). All 9 sites become one line.

**Blast radius:** 9 sites — mechanical substitution.

**Status:** [ ] proposed

---

#### B7 — Document `rpc::Server` singleton stop-flag constraint; decouple `dispatch.cpp` from `rpc::Server::stopRequested()`

**Principle:** 6 (Minimal surface)
**Severity:** LOW

**Current:** `gStopFlag` is a TU-global atomic in `rpc.cpp`; `requestStop()`/`stopRequested()` are static methods on `Server` but operate on this shared state. `dispatch.cpp` calls `rpc::Server::stopRequested()` to poll shutdown — coupling TUI dispatch to the RPC server's stop state rather than receiving a stop predicate via argument.

**Proposed:** Pass a `std::function<bool()> shouldStop` (or `std::atomic<bool>&`) into `dispatchTui`. Document the single-server-per-process invariant explicitly in `rpc.h`. Low urgency — the single-server invariant already holds in production.

**Blast radius:** 3 call sites in `dispatch.cpp`.

**Status:** [ ] proposed

---

### Delivery (delivery)

#### D1 — Extract `TokenWatcher`: remove transcript-scanning from `delivery::Loop`

**Principle:** 7 (Cohesion — god-object)
**Severity:** HIGH

**Current:** `Loop` owns `TokenScanState` per-agent map (lines 176–184 of `delivery.h`), incremental transcript-tail reads, CTX% computation, and atomic writes to `$STATE/status/<agent>.json` (lines 1471–1568 of `delivery.cpp`). None of this has any causal connection to record dispatch, ack, or retry. It fires on the same tick solely for scheduling convenience.

**Proposed:** Extract `TokenWatcher` class (or free function called from the broker's tick loop alongside `dl.tick()`). It needs only `cfg_.state_dir` and the live-agent list — no access to `in_flight_`, `blocking_ops_`, or `PolicyEngine`. Clean seam: reads transcripts, writes status files, touches nothing delivery-owns.

**Blast radius:** 1 call site in `tick()` + `TokenScanState` struct declaration. ~100 lines move to a new TU.

**Status:** [ ] proposed

---

#### D2 — Move `stampEpoch` / `recordEpoch` to `bus::msg` in `envelope.h`

**Principle:** 2 (Leaky boundaries), 3 (Free vs method)
**Severity:** HIGH

**Current:** `stampEpoch(Envelope&, uint64_t)` is literally `env.epoch = epoch`. `recordEpoch(Envelope)` is `return env.epoch`. Both live in `bus::delivery`, forcing `broker.cpp` — which has nothing to do with the delivery loop — to `#include delivery.h` solely for these three helpers. 15 call sites: 11 in `delivery.cpp`, 4 in `broker.cpp`.

**Proposed:** Move to `bus::msg` namespace in `envelope.h`/`envelope.cpp` as free functions or methods on `Envelope`. `broker.cpp` no longer needs to include `delivery.h`.

**Blast radius:** 15 call sites — mechanical namespace rename + include fix. No logic change.

**Status:** [ ] proposed

---

#### D3 — `emitAudit()` private helper: collapse 7-clone audit-emit blocks

**Principle:** 7 (Cohesion — repeated boilerplate)
**Severity:** MEDIUM

**Current:** 7 near-identical blocks in `delivery.cpp` (lines 425, 761, 926, 1105, 1253, 1383, 1443) each: construct an audit `TopicConfig`, getOrCreate the registry entry, open `bus::Journal` by raw path, construct an `Envelope` with `sender='broker'` + `stampEpoch` + protocol, append. The epoch-stamp is the omission-prone part; a future copy-paste miss produces the infinite-escalation loop warned about at line 775.

**Proposed:** Private method `auto Loop::emitAudit(std::string_view protocol, std::string_view body) → void`. All 7 sites collapse to one line. Epoch-stamp is baked in — can't be omitted.

**Blast radius:** 7 call sites within `delivery.cpp`. ~50 lines replaced by 7 one-liners. No external API change.

**Status:** [ ] proposed

---

#### D4 — `isAgentIdle()` private helper: collapse copy-pasted idle-gate check

**Principle:** 7 (Cohesion — duplicated inline block)
**Severity:** MEDIUM

**Current:** `dispatchAgentInbox` (lines 614–625) and `dispatchTuiCommands` (lines 713–724) each contain a byte-for-byte identical 10-line block: build `events_log` path, call `readAgents` with single-agent filter, call `paneStateCached`, call `computeState`, evaluate `State::Idle || (State::Starting && ...)`. A fix to the idle-gate logic must be applied twice.

**Proposed:** Private method `auto Loop::isAgentIdle(const std::string& agent, std::int64_t now) const → bool`. Both callers collapse to `if (env.deliver_when == 1 && !isAgentIdle(agent, now)) return;`.

**Blast radius:** 2 call sites within `delivery.cpp`. ~10 lines extracted.

**Status:** [ ] proposed

---

#### D5 — `inFlight()` targeted queries: narrow the exposed surface for non-debug callers

**Principle:** 6 (Minimal surface), 2 (Leaky boundaries)
**Severity:** MEDIUM

**Current:** `inFlight()` returns `const std::map<std::string, InFlight>&` — the entire internal map. `broker.cpp` uses it four ways: two `.contains(id)` point queries, one full iteration (debug `inflight` RPC), one `f.topic == tc.name` scan (GC drain-gate). The `InFlight` struct (with `cursor_after`, `attempts`, `next_retry_at`) leaks as delivery-internal bookkeeping into broker's coupling surface.

**Proposed:** Keep `inFlight()` for the debug-RPC path. Add `bool isInflight(std::string_view msg_id) const` and `bool hasInflightForTopic(std::string_view topic) const`. The three non-debug callers become single-line calls. Mark `inFlight()` `// debug/test only` in the header.

**Blast radius:** 3 broker call sites replaced by two new methods. Purely additive.

**Status:** [ ] proposed

---

#### D6 — `continuitySinceMs()`: make private or remove (zero external callers)

**Principle:** 6 (Minimal surface)
**Severity:** LOW

**Current:** `continuitySinceMs()` is public. `broker.cpp` reads `$STATE/continuity.ms` directly via `bus::readU64File` at line 621 — bypassing the method. Zero external callers.

**Proposed:** Make private, or remove.

**Blast radius:** 0 external callers. Single-line change in `delivery.h`.

**Status:** [ ] proposed

---

### Policy / Recovery (policy-recovery)

#### P1 — Recovery knobs: single `RecoveryBudgets` struct read once (see also C5)

**Principle:** 4 (Redundant data), 2 (Leaky boundaries)
**Severity:** MEDIUM

**Current:** `CLAUDE_BUS_STUCK_BUDGET_MS`, `CLAUDE_BUS_AUTO_CLEAR_MIN`, `CLAUDE_BUS_CLOCK_JUMP_MS` are each parsed independently in `recovery_actor.cpp` AND in `delivery.cpp`'s `maybeEscalateStuck`/`maybeAutoClear`/`updateContinuity`. Defaults can silently diverge.

**Proposed:** `struct RecoveryBudgets` (or extend `RecoveryThresholds`) with a factory that reads all knobs once. Pass the struct into both `RecoveryActor` (constructor) and the delivery `Loop` (via `PolicyContext` or config). C5 handles `RecoveryMode` itself.

**Blast radius:** 5 env-var read sites across 2 files.

**Status:** [ ] proposed

---

#### P2 — `would_recover_next_log_ms_`: typed pair key replaces `"agent\x1fsig"` composite string

**Principle:** 9 (Types), 6 (hard-to-misuse)
**Severity:** LOW

**Current:** `would_recover_next_log_ms_` in `recovery_actor.cpp` is keyed by `agent + "\x1f" + sig` at 3 construction/parse sites. `pruneDeadAgents` manually re-parses with `find('\x1f')`.

**Proposed:** `std::map<std::pair<std::string, std::string>, std::int64_t>`. Typed, never forgeable. `pruneDeadAgents` becomes `erase_if([&](auto& kv){ return !live.contains(kv.first.first); })`.

**Blast radius:** 3 sites, all within `recovery_actor.cpp`. Zero external impact.

**Status:** [ ] proposed

---

#### P3 — Hoist `kNoClearRoles` to a shared header

**Principle:** 7 (Cohesion)
**Severity:** LOW

**Current:** `{"comms", "primary"}` is declared as two separate static locals: `kClearSkip` in `recovery_actor.cpp:174` and `kSkipRoles` in `delivery.cpp:1047`. Same policy, two enforcement sites, manual sync required.

**Proposed:** `inline constexpr std::array<std::string_view, 2> kNoClearRoles{"comms", "primary"}` in `tty_policy.h` (which already holds the bedrock opt-out for `comms`). Both sites include it.

**Blast radius:** 2 definition sites across 2 files.

**Status:** [ ] proposed

---

### Pane / Status (pane-status)

#### PS1 — Typed predicates on `PaneState`: eliminate 8+ magic-string comparisons

**Principle:** 6 (Minimal surface), 9 (Types)
**Severity:** MEDIUM

**Current:** `PaneState::mode` holds `"INSERT" | "NORMAL" | "VISUAL" | "LOCKED" | "unknown"`. `bypass_perms` holds `"on" | "off" | ""`. `buffer` uses `"(empty)"` as a sentinel. 8+ raw string comparisons across `dispatch.cpp:72`, `delivery.cpp:624,723`, `agent_status.cpp:371,373,453`, `pane.cpp:625,628,638,644`.

**Proposed:** Add predicates: `isInsert()`, `isLocked()`, `modeKnown()`, `bypassOn()`, `bypassKnown()`, `hasBuffer()`. Keep raw string fields for the JSON/RPC wire path. Eliminate all business-logic string comparisons.

**Blast radius:** 8 comparison sites across 5 files + 3 sentinel assignments in `pane.cpp:701–702`. Mechanical — zero behavior change.

**Status:** [ ] proposed

---

#### PS2 — Move `agentColor` and `hasPresenceFile` out of the status-computation module

**Principle:** 7 (Cohesion), 3 (Free vs method)
**Severity:** MEDIUM

**Current:** `agentColor(name)` reads `~/.cache/claude-bus/agents/NAME.color` and returns an ANSI escape — a display concern. `hasPresenceFile(name)` reads `$STATE/presence/NAME` with a 1-hour expiry — an attachment-state concern. Both live in `agent_status.h`/`agent_status.cpp` alongside `computeAxes` and `readAgents`. `agentColor` constructs its path via `$HOME/.cache` directly, inconsistent with all other state I/O.

**Proposed:** Move `agentColor` to the `ansi.h` module extracted in C2 (display concern) or a `agent_display.h`. Move `hasPresenceFile` to the `state_paths.h` territory or a focused presence module. Fix `agentColor`'s path construction to use `stateRoot()` or the XDG cache helper.

**Blast radius:** `agentColor`: 3 call sites (`sub_agent_bar.cpp:178`, `sub_monitor.cpp:499`, `sub_log.cpp:320`). `hasPresenceFile`: 5+3 call sites in `delivery.cpp`, `broker.cpp`, `sub_lifecycle.cpp`.

**Status:** [ ] proposed

---

#### PS3 — `stateFrom()` inverse: add to `agent_status.h` (State label → enum)

**Principle:** 3 (Free vs method), 6 (Minimal surface)
**Severity:** HIGH — currently missing inverse

**Current:** `sub_monitor.cpp:50–65` defines a private `computeStateFromLabel(string_view) → State` that converts the broker's wire label back to the `State` enum. The forward direction (`stateName`, `stateGlyph`, `stateColor`) lives in `agent_status.h`. The inverse is absent from the public API. Any future viewer re-derives it.

**Proposed:** Add `auto stateFrom(std::string_view label) → State` to `agent_status.h` (declared) and `agent_status.cpp` (defined). `computeStateFromLabel` in `sub_monitor.cpp` becomes a call to it. The parallel axis inverses (`processAxisFrom`, `turnAxisFrom`) already set this precedent.

**Blast radius:** 1 current call site. Purely additive declaration; removes 15 local lines from `sub_monitor.cpp`.

**Status:** [ ] proposed

---

### Data model (data-model)

#### DM1 — `TopicConfig` dual representation: drop `kind_config` JSON blob, make `parsed_config` authoritative

**Principle:** 4 (Redundant data), 2 (Leaky boundaries)
**Severity:** HIGH

**Current:** `TopicConfig` carries both `json::Value kind_config` (raw JSON) and `TopicKindConfig parsed_config` (variant). The variant is populated in `topic_registry.cpp` but never read outside it — all callers reach past it to `kind_config.get("agent")`, `kind_config.get("subscribers")`, etc. The variant is dead abstraction.

**Proposed:** Drop `kind_config`. Make `parsed_config` authoritative. Add typed accessors (`agentName() → std::optional<std::string>`, `subscribers() → std::span<const std::string>`). Callers doing `kind_config.get("agent")` switch to `std::get<AgentInboxConfig>(cfg.parsed_config).agent`. Serialization writes from `parsed_config` at save time. Alternatively: drop `parsed_config` and keep `kind_config` — either way, the dual representation must go.

**Blast radius:** ~5 call sites in `delivery.cpp` and `broker.cpp` accessing `kind_config.get()`; plus serialization/deserialization in `topic_registry.cpp`. Single-cluster.

**Status:** [ ] proposed

---

#### DM2 — `isValidTopicName`: make private or anonymous-namespace

**Principle:** 6 (Minimal surface), 3 (Free vs method)
**Severity:** LOW

**Current:** `isValidTopicName(std::string_view)` is a public free function in `topic_registry.h`. It is called only inside `topic_registry.cpp` (two sites). Zero external callers.

**Proposed:** Zero external callers, used only inside `topic_registry.cpp` — move it to an **anonymous namespace** in that .cpp (file-local), *not* a `static` member. A static member would add to `TopicRegistry`'s surface for a function nobody outside calls; minimal-member-API says keep it off the class.

**Blast radius:** 0 external call sites. Header-only change.

**Status:** [ ] proposed

---

### JSON (json)

#### JN1 — `as*` accessors: fatal precondition checks on wrong-type access (see C3)

**Principle:** 5 (Crash vs Result)
**Severity:** HIGH

**Current:** `asBool()`, `asInt()`, `asString()` silently return zero/empty on wrong type. `asArray()`/`asObject()` dereference null `shared_ptr` with no check — UB/crash with no diagnostic. 30 call sites across 12 files assume the guard was done upstream.

**Proposed:** Add `fatal("json::Value::asArray called on {}", typeStr())` (always-on) to each accessor. This is the specific application of the cross-cutting C3 fix to `json_min.cpp`.

**Blast radius:** 30 call sites — unaffected when guard is correct. Broken sites become loud crashes.

**Status:** [ ] proposed (subsumed by C3)

---

#### JN2 — Drop mutable `asArray()`/`asObject()` overloads: zero callers, aliasing trap

**Principle:** 6 (Minimal surface)
**Severity:** MEDIUM

**Current:** `arr_` and `obj_` are `shared_ptr`. The non-const `asArray()&` and `asObject()&` return writable references into shared storage — mutating through a copy silently mutates all aliases. Zero external call sites actually mutate through these overloads.

**Proposed:** Drop the non-const overloads from `json_min.h` and `json_min.cpp`. Removes the aliasing trap. Zero blast.

**Blast radius:** 0 external callers. Two overloads deleted from `json_min.h`/`.cpp`.

**Status:** [ ] proposed

---

#### JN3 — Move `okResponse`/`errorResponse` from `bus::json` to `bus::rpc`

**Principle:** 2 (Leaky boundaries)
**Severity:** MEDIUM

**Current:** `okResponse()` and `errorResponse()` live in `bus::json` and hard-code `"ok"`/`"error"` RPC field names — a protocol contract — inside the domain-agnostic JSON serializer. 47 call sites in `broker.cpp` and `sub_*.cpp` treat them as universal JSON vocabulary.

**Proposed:** Move to `bus::rpc` in `rpc.h`/`rpc.cpp`. `bus::json` stays protocol-agnostic. The 47 call sites already include `rpc.h` transitively — change is a namespace qualifier update only.

**Blast radius:** 47 call sites — namespace rename, no logic change.

**Status:** [ ] proposed

---

#### JN4 — Delete `Value::null_()` redundant named constructor

**Principle:** 6 (Minimal surface)
**Severity:** LOW

**Current:** `Value()` default-constructs a Null-typed value. `Value::null_()` does the same with one extra assignment. Two spellings, no semantic distinction. 4 call sites in `event.h`, `topic_registry.h`, `broker.cpp`.

**Proposed:** Delete `null_()`. Replace 4 call sites with `Value{}`.

**Blast radius:** 4 sites — mechanical.

**Status:** [ ] proposed

---

### CLI / sub layer (cli)

#### CL1 — `bus::callerAgentId()`: promote duplicated `senderFromEnv()` to `agent_status.h`

**Principle:** 3 (Free vs method), 6 (Minimal surface)
**Severity:** MEDIUM

**Current:** `senderFromEnv()` reading `CLAUDE_BUS_AGENT_ID` is defined in anonymous namespaces in `sub_produce.cpp:26` and `sub_tasks.cpp:57`. `sub_consume.cpp:294` inlines the same 3-liner a third time without extracting it. Three definitions of one 3-line function.

**Proposed:** Promote to `bus::callerAgentId()` in `agent_status.h`/`agent_status.cpp` alongside the existing `nowMs()`. One definition, three callers import it from the header they already include.

**Blast radius:** 3 call sites in `sub/`. Pure additive + deletion.

**Status:** [ ] proposed

---

#### CL2 — Move `subTask` and `subDone` out of `sub_produce.cpp` to `sub_tasks.cpp`

**Principle:** 7 (Cohesion)
**Severity:** MEDIUM

**Current:** `sub_produce.cpp` contains broker-RPC message producers (`subEnqueue`, `subMail`, `subBroadcast`, `subSlash`) AND `subTask` (writes `$STATE/title/<agent>` directly) AND `subDone` (appends to `$STATE/done/<agent>.jsonl` directly). The latter two are durable file writers, not message producers. Their broker-independence is architecturally significant and is obscured by their placement alongside enqueue logic.

**Proposed:** Move `subTask` and `subDone` to `sub_tasks.cpp` (or a new `sub_state_write.cpp`). The architectural note that these intentionally bypass the broker sits next to both writers.

**Blast radius:** Declarations in `sub.h` and `bus.cpp` registry unchanged. ~100 lines relocate across files.

**Status:** [ ] proposed

---

#### CL3 — `splitCsv`: shared utility replacing 3 inline hand-rolled parsers

**Principle:** 3 (Free vs method), 6 (Minimal surface)
**Severity:** LOW

**Current:** Three separate hand-rolled CSV parsers: named `splitCsv` in `sub_tasks.cpp:107` and two open-coded for-loops in `sub_produce.cpp:294–303` (broadcast `--to`) and `sub_topic.cpp:127–137` (`--subscribers`). `truncate()` is duplicated between `sub_tasks.cpp` and `sub_verify.cpp`.

**Proposed:** `auto splitCsv(std::string_view) → std::vector<std::string>` in a new `src/sub/sub_util.h`. `truncate()` moves there too.

**Blast radius:** 3 inline parsers + 2 `truncate` definitions → calls. Low risk — all anonymous-namespace free functions.

**Status:** [ ] proposed

---

#### CL4 — Fix `bus msg` unknown-verb exit code: return 2, not 1

**Principle:** 5 (Error contract consistency)
**Severity:** LOW

**Current:** `bus msg <unknown>` exits 1 (operational failure). `bus broker`, `bus tasks`, `bus topic` all exit 2 (usage error) for unknown sub-verbs. Inconsistent — a script testing `$? == 2` for usage errors gets wrong results on the `msg` path.

**Proposed:** `return 2` at `sub_msg.cpp:42`. Convention: exit 2 = programmer/usage error; exit 1 = operational failure.

**Blast radius:** 1-line change.

**Status:** [ ] proposed

---

### Foundations (foundations)

#### F1 — Drop `ErrorCode` enum: `Error` reduces to a tagged string; nobody reads `.code`

**Principle:** 6 (Minimal surface)
**Severity:** MEDIUM

**Current:** `Error` in `types.h` carries `ErrorCode` (NotFound/ParseError/IOError/InvalidArgument/Unknown) and a string message. 80+ `std::unexpected` construction sites use the message-only ctor; no call site reads `.code` or branches on `ErrorCode` variants. `ErrorCode::Unknown` is the default and the only value ever populated.

**Proposed:** Drop `ErrorCode` and the two-field struct. Replace with `struct Error { std::string message; }` (or a named `using Error = std::string` with a thin wrapper for future extensibility). Introduce `ErrorCode` again only when the first match-site is written. Until then the enum is a false promise.

**Blast radius:** `types.h` only; no call site reads `.code`. The message-only `Error(std::string)` ctor is already the only one in use.

**Status:** [ ] proposed

---

#### F2 — Delete `Error()` default constructor

**Principle:** 5 (Crash vs Result)
**Severity:** LOW

**Current:** `Error()` zero-initialises to `ErrorCode::Unknown` + empty message. Silent propagation of a blank error through `Result<T>` is undetectable — looks like a real operational failure with no description.

**Proposed:** `= delete` the default constructor. Every `Error` must carry a non-empty message. Sites needing an unset sentinel use `std::optional<Error>`.

**Blast radius:** Zero construction sites use `Error()` without arguments (grep confirms).

**Status:** [ ] proposed

---

#### F3 — `installInterruptHandlers()`: shared stop-flag + zero-argument overload

**Principle:** 6 (Minimal surface)
**Severity:** LOW

**Current:** 7 viewer TUs (`sub_inbox`, `sub_monitor`, `sub_log`, `sub_events`, `sub_tasks`, `sub_agent_bar`) + `rpc.cpp` each independently declare `volatile sig_atomic_t gStopXxx = 0` and `auto onSignalXxx(int) → void { gStopXxx = 1; }`, then pass it to `installInterruptHandlers`. The three-part pattern must be replicated correctly each time.

**Proposed:** `signals.h` owns a canonical `volatile sig_atomic_t gStop = 0` and `onStop(int) { gStop = 1; }`, paired with a zero-argument `installInterruptHandlers()`. TUs that need a custom handler keep the existing overload. Removes 6 identical copy-paste triads from viewer TUs.

**Blast radius:** 7 call sites — each is a 3-line deletion + reference update. `rpc.cpp`'s atomic variant stays as is.

**Status:** [ ] proposed

---

## Completeness-critic addenda

A second pass checked every source file against the review. It found real gaps the area reviewers missed, and flagged three proposals as over-reaching the "minimum code that solves the problem" bar. Folded in here rather than rewriting the items above.

### Gaps the first pass missed

#### G1 — `sub_lifecycle.cpp` decomposition (587 lines — biggest CLI file, zero first-pass attention)

**Principle:** 7 (Cohesion), 3, 2 · **Severity:** MEDIUM · **Wave:** 2–3

`sub_lifecycle.cpp` holds `subRecover` / `subSpawn` / `subRestorePeers` / `subDespawn` — four substantial verbs — plus its own `selfDir()` (see G2) and raw `stateRoot() + "..."` concatenations in three dynamic-peers spots (folds into C4). No reviewer touched it. Pass: split the four verbs if they don't share state, route its path-building through C4's accessors, dedup `selfDir`.

#### G2 — `selfDir()` duplicated in `sub_lifecycle.cpp` + `sub_roles.cpp`

**Principle:** 3 (Free / static), 7 · **Severity:** LOW · **Wave:** 1 (with CL3)

Identical `/proc/self/exe` logic, two anonymous-namespace copies. One generic helper in the `sub_util.h` that CL3 introduces.

#### G3 — `tail_reader.h` pending migration left unfinished

**Principle:** 7 (Cohesion) · **Severity:** MEDIUM · **Wave:** 2 (with D-series)

`delivery.cpp` carries three inline copies of tail-read logic that `tail_reader.h` was created to absorb (a documented but incomplete Tier-1.8 migration). Finish it: move the three inline copies onto the component.

#### G4 — `RecoveryActor` is a softer partial object

**Principle:** 1 (Partial objects) · **Severity:** LOW–MEDIUM · **Wave:** 3 (with policy)

Constructed with paths that are lazily loaded on first use — a milder form of the J1 pattern. Audit whether any method's validity depends on construction state; if so, make it total.

#### G5 — `recovery.h` pure-unit surface unexamined

**Principle:** 3 (Free / static), 6 · **Severity:** LOW · **Wave:** 2

The `RecoveryLedger` serialization pair (`ledgerToJson` / `ledgerFromJson`) and `nowMonoMs` placement got no scrutiny. Check: does serialization belong on the type (it's for one type → static member), and is `nowMonoMs` a generic time helper that belongs with C2's `time_util.h`.

#### G6 — `Event::payload` escape-hatch couples `event.h` to `json_min`

**Principle:** 2 (Leaky boundaries) · **Severity:** LOW · **Wave:** 2

`Event` carries a `json::Value payload` — a typed escape hatch that drags the JSON lib into every event consumer. Examine whether the payload should be opaque bytes or a narrower typed field.

#### G7 — `policy.h` `PolicyContext` / `AgentSnapshot` got no scrutiny of their own

**Principle:** various · **Severity:** MEDIUM · **Wave:** 2–3

Only touched as a blast-radius note in C2 (the `agent_status.h` include). These are the policy seam's core types — they deserve a dedicated structural pass before the policy-as-coordination-substrate work builds on them.

### Overreach flags — prefer the simpler first cut

- **B2** (`runBroker` → `BrokerState` + five `registerX` groups): a wholesale decomposition of a 950-line function into ~6 units — a structural *preference*, not a fix. Simpler first cut: extract `getOrOpenLog` and `recordToJson` as file-scope helpers in `broker.cpp`; introduce handler groups only if a real reuse case emerges. Re-scope before scheduling.
- **C4** (`StatePaths` full path vocabulary): a thinner fix covers the misspelling class — inline helpers for the most-repeated paths (`brokerSock`, `topicLog`, `presencePath`) rather than a dozen accessors that must track every path as it evolves. Blast radius understated: `sub_agents.cpp`, `sub_lifecycle.cpp`, `sub_consume.cpp` each have their own concatenations not in the 23-site count.
- **F3** (shared stop-flag): the one-`gStop` overload quietly couples TUs that are independently stoppable today (safe in production — one subcommand per process — but uncosted). Simpler: a small `signals.cpp` TU owns `gStop` + `onStop`, or leave per-TU. Name the coupling before adopting.

### Confirmed out of scope (genuinely clean)

`blackboard_actor`, `dispatch_actor` (small + clean), `crc32c`, `journal_internal.h`, `bus_main.cpp`, `pane_state.h`, and the thin RPC-wrapper subs (`sub_broker`, `sub_meta`, `sub_pane`, `sub_state`).

---

## Recommended sequence

Grouped into three waves. Items in the same wave can run in parallel; each wave unlocks the next. (G-items carry their suggested wave inline above.)

### Wave 1 — Quick crisp wins (low blast, high clarity payoff)

These are additive or confined to one file. No downstream breakage. Good first pass.

| # | Item | Rationale |
|---|------|-----------|
| C3 | Unify `fatal()` crash contract | Foundation for J4, JN1; 2 lines in envelope.cpp + 6 in json_min |
| J3 | Delete `Journal::cursorToToken/cursorFromToken` wrappers | 9-site rename, zero risk |
| JN2 | Drop mutable `asArray()/asObject()` | 0 callers, removes aliasing trap |
| JN4 | Delete `Value::null_()` | 4-site mechanical substitution |
| B3 | `dispatch.h`: drop `BrokerConfig`, pass `state_dir` | 3 sites, trivial decoupling |
| B5 | `Journal::cursorsDir()` static | 1 call site, Journal owns its layout |
| B7 | Document rpc::Server singleton; pass stop-predicate to `dispatchTui` | 3 sites, removes hidden coupling |
| D5 | `inFlight()` targeted queries | Additive; narrows surface immediately |
| D6 | `continuitySinceMs()` → private | 0 callers; 1-line fix |
| PS1 | `PaneState` typed predicates | 8 string comparisons → predicates |
| PS3 | `stateFrom()` inverse in `agent_status.h` | Additive; removes a local re-derivation |
| P2 | `would_recover_next_log_ms_` typed pair key | 3 sites, self-contained in recovery_actor.cpp |
| P3 | Hoist `kNoClearRoles` to `tty_policy.h` | 2 definition sites |
| CL4 | `bus msg` exit code 2 | 1-line fix |
| F2 | Delete `Error()` default ctor | 0 callers |
| F3 | `installInterruptHandlers()` shared stop-flag | 7 call sites, removes boilerplate triads |
| CL3 | `splitCsv` shared utility | 3 inline parsers → calls |
| DM2 | `isValidTopicName` → private | 0 external callers |
| JN4 | `Value::null_()` → `Value{}` | 4 sites |

### Wave 2 — Structural mid (medium blast, single-cluster, high value)

Contained changes with clear boundaries. Each is a well-scoped refactor of one or two files.

| # | Item | Rationale |
|---|------|-----------|
| C2 | `ansi.h` + `time_util.h` extract | Breaks the `agent_status.h` dependency magnet; ~16 include-path swaps |
| C4 | `StatePaths` named accessors | 23 scattered `stateRoot() + "..."` calls → typed paths; fixes the `sub_roles.cpp` leak |
| C5 | `TaskState` + `Boundary` + `RecoveryMode` enums | 3 bounded-domain raw strings → typed enums; ~30 sites total |
| C6 | `PaneId` strong type | 17-site wrapping; catches agent-name/pane-id transpositions at compile time |
| B4 | `TopicConfig` kind predicates | 16 comparison sites; prerequisite before C1 |
| B6 | `TopicConfig::makeAudit()` factory | 9 sites; enforces kind↔config invariant |
| D2 | `stampEpoch`/`recordEpoch` → `bus::msg` | 15 sites; removes broker's dependency on `delivery.h` |
| D3 | `Loop::emitAudit()` private helper | 7 clone sites → 1 definition |
| D4 | `Loop::isAgentIdle()` private helper | 2 clone sites → 1 definition |
| P1 | `RecoveryBudgets` single-read struct | 5 env-read sites → 1 factory |
| PS2 | Move `agentColor`, `hasPresenceFile` out of status core | ~8 call sites; separates display/presence from computation |
| CL1 | `bus::callerAgentId()` in `agent_status.h` | 3 duplicate definitions → 1 |
| CL2 | `subTask`/`subDone` → `sub_tasks.cpp` | Clarifies broker-bypass architecture |
| JN3 | `okResponse/errorResponse` → `bus::rpc` | 47 sites; protocol contract leaves JSON layer |
| F1 | Drop `ErrorCode` from `Error` | `types.h` only; removes dead interface surface |

### Wave 3 — Drastic structural moves (high blast, highest long-term value)

These reshape major interfaces. Do after Wave 2 has stabilised the call sites they touch.

| # | Item | Rationale |
|---|------|-----------|
| J1 | `CursorStore` extraction | Eliminates the partial-object Journal; prerequisite for B1 |
| J2 | `Journal::open`/`create` factories | Eliminates silent lazy creation; ~30 construction sites |
| B1 | Broker log cache consolidation | Requires J1; eliminates ephemeral cursor re-opens in 6 handler paths |
| B2 | `runBroker` → `BrokerState` + handler groups | Breaks the 950-line single-function god-closure; self-contained blast |
| C1 | `TopicKind` enum | Requires B4 as stepping stone; eliminates 39 raw string comparisons; delivery dispatch becomes exhaustive switch |
| D1 | `TokenWatcher` extraction from `Loop` | Requires D3/D4 first; removes observability concern from delivery god-object |
| DM1 | `TopicConfig` dual-rep: drop `kind_config` | Requires C1 first; collapses ~5 raw JSON accessor sites onto typed `parsed_config` |
| JN1 | `json as*` fatal preconditions | Subsumed by C3 but requires audit of all 30 call sites |
