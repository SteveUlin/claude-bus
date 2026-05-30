# claude-bus project roadmap

The forward-looking plan: what is built, and what to build next. Companion to
`docs/improvement-roadmap.md` (which carries the per-item mechanism + deep-doc
reasoning). This doc folds the recent landings and three fresh surveys into one
dependency-ordered task list of **still-open work only**.

---

## Project overview

`claude-bus` runs one human and many Claude Code agents as independent
processes inside zellij panes, coordinated by a **singleton, out-of-context
C++23 broker daemon**. The architecture maps to four directories: `layouts/`
(zellij KDL pane topologies — the user-facing API), `bin/` (the unified
`bin/bus` binary plus the `agent-launch` shell), `settings/` (canonical Claude
Code config + shared hooks), and `coordination/` (layerable patterns).

The moat is the broker as **durable system-of-record**: it owns the topic
registry, append-only logs, per-consumer cursors, a 250 ms delivery loop, and
the in-flight / retry / ACK / audit / `inbox-human` escalation path — all
*outside* every agent's context, costing zero context tokens and surviving
compaction. That is the differentiator versus context-coupled agent-messaging.

Recently landed and treated as done: **off-TTY delivery as the fleet default**
(the broker drains an agent's inbox into `UserPromptSubmit` `additionalContext`
instead of racing the pane's PTY; opt-out compiled into `src/tty_policy.h`, with
`comms` on the bedrock TTY path); a **doorbell + strand-watchdog** to wake idle
off-TTY agents; **durable `$STATE`** on an XDG root (survives reboot);
**deterministic config materialization** from landed `main` per launch;
**`bus_core` + unit tests**; **fleet-wide OTel**; a **statusline sidecar**; and
**learnings injection** at `SessionStart`.

What remains splits into a correctness floor (idempotency, exact ACK, cursor
identity), durability follow-through (log rotation, deterministic *code*
propagation, off-TTY verification), resiliency hardening, and the
orchestration/learning capabilities that the substrate now unlocks.

---

## Next work (still-open only, dependency-ordered)

### Tier 0 — Correctness floor (do first; these are latent bugs on the now-default path)

| ID | Project/Task | Scope | Depends-on | Effort | Payoff | Lane |
|----|--------------|-------|------------|--------|--------|------|
| C1 | Fix agent-inbox cursor namespace split | `fetch`/`peek` (`broker.cpp:444,612`) accept arbitrary `--consumer` and write `<name>.cursor`, but delivery + drain hardcode `consumer=""` → `_default.cursor`. Live proof: `inbox-kvothe`/`inbox-bast` carry both with divergent offsets. For single-recipient kinds, normalize `--consumer` to `""` (or reject it). `is_agent_inbox` is already known at the call site. | — | low | very-high | kvothe |
| C2 | Consumer-side dedup on existing `msg_id` | `msg_id` is minted (`{sent_ms}-{sender}-{rand}`) but never checked. Record last-acked-msg_id per (topic,consumer) at the drain; skip re-delivery of an already-seen id. ~15 LOC. The idempotency floor everything else leans on. | — | low | very-high | kvothe |
| C3 | State derivation off `extractField` onto `json_min` | `agent_status.cpp:30` readAgents uses flat-substring `extractField`; a payload-nested `agent`/`event` key shadows the top-level one (silent misparse). `json_min` already ships in-tree. | — | low | high | elodin |
| C4 | Off-TTY + doorbell objective verification | Assert end-to-end: (a) doorbell-wake audit fires for an idle off-TTY agent with queued mail; (b) strand-watchdog stays silent under healthy delivery; (c) `config == landed main` per agent post-launch; (d) exactly one delivery path fires per agent (off-TTY via additionalContext, `comms` via TTY) — no double-delivery. Pure-logic (computeAxes readiness) → `tests/unit`; delivery-path → `tests/off-tty-itest.sh`. | C1 | low-med | very-high | bast |

### Tier 1 — Exactness + durability follow-through

| ID | Project/Task | Scope | Depends-on | Effort | Payoff | Lane |
|----|--------------|-------|------------|--------|--------|------|
| D1 | Rotate/cap `events.jsonl` + audit log | `$STATE` is now durable, so `events.jsonl` (~25 MB live) and `audit.log` grow unbounded, slowing boot scan + monitor + broker startup. Add an in-tick size/age trim+roll (events.jsonl is advisory; the binary topic log is canonical, so trimming is safe). | — | low | high | elodin |
| D2 | Enforce `retention_ms` / per-topic depth cap | `retention_ms` and `max_record_bytes` are declared + parsed (`topic_registry.h:52-53`) but **nothing enforces them** — dead config. Add an in-tick retention step (age + depth) so `work-queue` and `audit` topic logs stop growing unbounded. (Roadmap 3.7.) | — | low | med | elodin |
| D3 | Make the ACK carry the `msg_id` | `delivery.cpp:366-396` does a positional join (ack oldest in-flight for this agent on next `UserPromptSubmit`) — the mid-stream-dropped-turn confusion. Have the drain hook emit `{event:bus-ack,msg_id}`; broker acks by id, not position. The drain hook (off-TTY default) is now the natural emit point. | C2 | med | very-high | kvothe |
| D4 | Deterministic CODE propagation | 1b made *config* materialize from landed `main`; the `bin/bus` binary, `agent-launch`, and `fleet.kdl` still run from the default workspace checkout (shows divergent in `jj workspace list`) and need a manual reconcile+rebuild after a land. Extend the materialize-from-main pattern (or a pre-launch build-from-main step) to code, or automate the reconcile+rebuild. | — | med | high | bast |
| D5 | Mirror escalate-on-stale-epoch in the drain path | TTY push escalates a stale-epoch record to audit + inbox-ops; the drain pull path (now the default) only *drops* it silently (`broker.cpp:670`). Mirror the escalate, or consciously document the asymmetry. | C4 | low | low-med | bast |
| D6 | Extract one `TailReader` (wire it in) | `tail_reader.h` exists + is unit-tested, but `scanEvents`/`maybeScanTokens`/topic `parseFrom` still each reimplement resume-from-offset (drift + tellg text-mode coupling). Migrate them onto it. | — | med | med | elodin |
| D7 | Typed `Event` struct parsed once | `extractField`/`extractStr` scanners duplicated across `agent_status.cpp`, `sub_events.cpp`, `sub_monitor.cpp` with divergent escape handling. Parse each event line once into a typed struct. Subsumes C3; prerequisite for trace work. | C3 | med | high | elodin |
| D8 | computeAxes as a true fold (decouple from wall-clock) | readAgents overwrites `info.last` per line (`state = f(last_event)`), so a planned-but-never-emitted tool call stays invisible — the dropped-turn root cause. Carry an accumulator (open-tool / work-start); use a timerfd for escalation timing, not wall-clock comparison. | C3, D7 | med | high | elodin |

### Tier 2 — Resiliency hardening

| ID | Project/Task | Scope | Depends-on | Effort | Payoff | Lane |
|----|--------------|-------|------------|--------|--------|------|
| R1 | Triage table for auto-recovery | `maybeAutoClear` (`delivery.cpp:805-905`) is one-off control flow. Turn it into a data-driven `(signature, action, guard)` table; add STUCK→nudge and high-CTX→/compact rows. Signatures already computed in `agent_status.h`. | — | med | very-high | kvothe |
| R2 | OTP-style restart-intensity guard (MaxR/MaxT) | An anti-thrash invariant on auto-recovery so the triage table can't nudge/respawn a broken agent forever. | R1 | low | high | kvothe |
| R3 | Atomic claim + documented modes for work-queue | `fetch` silently flips load-balance (shared `_default`) vs fan-out (per-consumer); single-assignment relies on single-threaded RPC, not the data model. Add a claim/status CAS field + documented modes. | — | med | high | kvothe |
| R4 | Generalize in-flight tracker into a real PEL | Unify in-flight map + blocking-op map + retry timers into one `(topic,consumer,msg_id,claimed_at,delivery_count)` model with XAUTOCLAIM-style idle-reclaim; gives work-queue crash recovery. | C2, D3 | med | high | kvothe |
| R5 | Escalating backoff in scanRetries | Flat `now + ackTimeoutMs` hammers a wedged agent 3× at fixed cadence (`delivery.cpp:762`); space attempts 1×/2×/4×. | — | low | med | kvothe |
| R6 | NAK event channel | `{event:bus-nak,msg_id}` on blocking-op/not-ready so the broker re-dispatches at the next idle boundary instead of burning three blind 60 s timeouts. | D3 | low | med | kvothe |
| R7 | Active replay-on-restart | SessionEnd releases in-flight without re-sending (passive). Active re-send on respawn, gated by the idempotency key. | C2, C4 | med | high | bast |
| R8 | PreToolUse deny hook for coordinators | The PreToolUse block only logs; the "no file edits" rule for comms/auri is prose-only under fleet-wide `--dangerously-skip-permissions`. Add a scoped `permissionDecision: deny`. | — | low | med | bast |

### Tier 3 — Orchestration & learning (capabilities the substrate now unlocks)

| ID | Project/Task | Scope | Depends-on | Effort | Payoff | Lane |
|----|--------------|-------|------------|--------|--------|------|
| O1 | Gather/join barrier on the `correlation` field | `topic_log.h:61` `correlation` (u128) is read by nothing. Add a `results-<corr>` topic + a gather-complete-on-quorum/timeout primitive. Keystone for map-reduce + judge panels. | D8, R4 | high | very-high | bast |
| O2 | Reflection → per-agent `learnings.md` (write side) | Read path (inject-learnings.sh) landed; the distillation side is missing. A `claude -p` reflector over transcripts (or broker-drained continuous capture), with a co-designed prune/consolidate path. | — | med | high | bast |
| O3 | Promotion ladder: episodic → learnings → roles → CLAUDE.md | Read path exists; add the recurrence+survival promotion mechanism that accretes durable lessons upward. | O2 | low | high | bast |
| O4 | `bus learnings append` write verb | A model-driven verb to commit a lesson at the moment of insight. | O2 | low | med | bast |
| O5 | Wire the inert PreCompact hook to checkpoint | PreCompact slot exists but is inert; snapshot episodic detail to a file before compaction (the moment richest in context). Feeds O2. | — | low | med | bast |
| O6 | Specialist long-lived roles (supervisor / judge / librarian) | No such roles exist. Librarian unifies reflection + cross-agent promotion + novel-failure triage; judge backs verify-and-converge. | O1, O2 | med | high | bast |
| O7 | Typed-completion pipeline gate (`deliver_after=<corr>`) | Broker releases stage B on observing A's `Stop` for a correlation — a robust stage-gate. | D8, O1 | med | high | bast |
| O8 | Judge-panel / verify-and-converge over panes | Orchestration reusing broadcast + inbox + mail, gated on the gather barrier. | O1, O6 | med | high | bast |

### Tier 4 — Exploratory (deferred; do on measured need)

| ID | Project/Task | Scope | Depends-on | Effort | Payoff | Lane |
|----|--------------|-------|------------|--------|--------|------|
| X1 | Per-protocol guarantee selection | Named guarantee per protocol: idempotent → at-least-once + dedup; destructive (/clear,/compact) → at-most-once-preferred. | D7 | low | med | kvothe |
| X2 | `bus msg redrive` (DLQ as a loop) | A re-enqueue-by-msg_id verb so a dead message is more than an audit obituary. | — | low | med | kvothe |
| X3 | Token-anomaly detection in delivery loop | A `maybeDetectAnomaly` sibling to `maybeScanTokens` tracking token-delta/llm-call anomalies (OTel or transcript inputs). | — | med | high | elodin |
| X4 | Tier-5 exploratory set | `bus trace` span-tree viewer, windowed-pread `peek`, CQRS read-model, keyed compacting blackboard, signalfd+epoll reactor, importance×recency learnings retrieval, `bus topic verify`, restartable coordinator. All deferred by design. | various | high | med | — |

---

## Critical path

The dependency spine runs through **identity → exactness → the join barrier**.

1. **C1 + C2** are the unblockers. C1 (cursor namespace split) directly causes
   lost/duplicated mail on the human-facing `comms` inbox today; C2 (dedup on
   `msg_id`) is the idempotency floor that D3, R4, R7, and X1 all assume. Both
   are low-effort and depend on nothing — do them first.

2. **C2 → D3** (msg_id-carrying ACK) replaces the positional ACK join, killing
   the mid-stream-dropped-turn confusion class. D3 + C2 then unblock **R4** (the
   real PEL with idle-reclaim) and **R6/R7** (NAK channel, active replay).

3. **C3 → D7 → D8** is the parse-correctly spine: move off `extractField`, parse
   each event once into a typed struct, then make computeAxes a true fold. D8 is
   what finally makes a planned-but-unemitted tool call *visible* — and it is the
   prerequisite for the orchestration tier.

4. **D8 + R4 → O1** (the gather/join barrier on the dormant `correlation`
   field). O1 is the keystone: it unblocks O7 (pipeline gate), O8 (judge panel),
   and the judge/supervisor specialist roles in O6.

5. The learning ladder (**O2 → O3 → O4**, fed by O5) is independent of the
   delivery spine and can run in parallel on the `bast` lane.

6. **C4** (off-TTY/doorbell verification) gates D5 and R7; run it early since
   off-TTY is the fleet's default delivery path and an unverified default is the
   highest-blast-radius gap. **D1/D2** (log rotation, retention) are independent,
   low-effort latency wins — schedule them whenever a lane has slack.
