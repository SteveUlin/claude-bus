# review-findings.md — claude-bus review run, 2026-06-06

Produced by `workflows/review.js` (mode=review) over `main` (`0a47b605`), then
re-bucketed by hand on resume. **For sulin's review** — auri routes.

> **Status update (post-review landings).** Several PROPOSE items have since
> shipped: `delivery-0-0` (double-dispatch), `delivery-2-0` (payload leak), and
> `rpc-1-0` (backpressure doc) landed on `main 3f174ff8` (elodin). `kernel-0-0`
> (the HIGH torn-record) is the last fix in progress (elodin). The 4 APPLY-NOW
> cleanups below landed separately (chronicler). The pipeline is working — read
> the PROPOSE entries as the original recommendation, not the current state.

## Provenance + how to read this

- **33 raw findings → 12 survived** adversarial verify (3 distinct-lens
  skeptics per finding, majority-refute kills).
- The synthesis stage **hit the session rate limit**, so the workflow fell to
  its deterministic floor (`tier=floor`). The floor preserves every finding in
  the return value (the hardened-synth invariant held — that's the point) but
  **cannot compute the APPLY-NOW/PROPOSE split** — it dumped all 12 into
  PROPOSE. **I did the bucketing by hand on resume**, which is better than an
  LLM synth would have done anyway.
- **Verifier-failure caveat:** many verifier subagents completed without
  emitting `StructuredOutput`, so some findings show `votes:0, refutes:0` —
  meaning their verifiers *all failed*, not that they were cleanly confirmed.
  My `verifyFindings` wrongly treats that as `survives:true` (a real bug in the
  tool — see *Tooling gaps* below). **I re-verified every finding I acted on
  against the actual code myself**; verification status is noted per finding.

Buckets: **4 APPLY-NOW (applied + committed) · 6 PROPOSE · 2 REFUTED.**

---

## APPLY-NOW — applied + committed (jj `3b97f547`, build + ctest green)

All four self-verified against current `main`, surgical, low-risk.

### 1. `policy-0-0` — work-queue claim dropped on inbox-append failure *(was HIGH)*
`src/delivery.cpp:1288` `executePolicyAction` discarded the append result
(`auto _ = log.append(...)`) then ran `consumeQueueHead` **unconditionally**
(`:1295`). `TopicLog::append` genuinely returns `std::unexpected` on open/write
failure (`topic_log.cpp:300-305`). So a failed inbox append still advanced the
work-queue cursor past the task → **permanent silent task loss**, violating the
at-least-once invariant the inline comment *and* `docs/work-queue-dispatch.md
§2(A)` both promise. The sibling `getOrAutoCreate`-fail path already returns
before consuming — this path didn't.
**Fix:** gate the consume on append success. Self-verified real.

### 2. `delivery-1-2` — in-flight tmp orphan + unfiltered load *(low)*
`writeInflight` (`:262-266`) skipped removing its `.tmp` on write failure
(diverging from the atomic-write discipline used elsewhere in the same file).
`load()` (`:223`) iterated **every** regular file with no extension filter, so a
stray `<id>.json.tmp` could be parsed into a phantom in-flight entry on restart.
**Fix:** `fs::remove(tmp)` on failure; skip non-`.json` entries in `load()`.
(Verified `inflightPath` ends in `.json`, so the filter is safe.)

### 3. `cli-0-0` — `bus introduce` hint advertises a command that prints nothing *(was HIGH)*
The hint (`sub_agents.cpp:201,224`) advertised `bus events --agent X --since 30m`,
but `bus events --since` (`sub_events.cpp:101`) is a raw lexicographic compare
`ev->ts < since`. `"2026-..." < "30m"` is always true (`'2' < '3'`), so
`--since 30m` **silently drops 100% of lines** — reads as "no activity." A
"monitor-must-not-lie" bug. Fully verified (3/0 verifiers).
**Fix applied (surgical):** drop the broken `--since 30m` from the hint.
**Deeper fix → PROPOSE #6** (make `--since` actually parse durations).

### 4. `cli-0-3` — subcommand registry not alphabetical *(low, cosmetic)*
`bus.cpp` listed `spawn, restore-peers, despawn` out of order, contradicting the
comment's own "alphabetical-by-surface order" invariant → misordered `bus help`.
**Fix:** moved `despawn` after `broker`, `restore-peers` after `recover`.
Verified `bus help` output is now fully alphabetical.

---

## PROPOSE — real, but design-touching / higher-risk / needs your call

### 5. `delivery-2-0` — large-body payload files never deleted *(medium, resource)*
`$STATE/payloads/<id>.body` is written for every >1KB mail (`delivery.cpp:628`)
but **no code path ever removes one** (verified: no `fs::remove` on a payloads
path anywhere; `broker.cpp` explicitly preserves `payloads/` across boots;
`bus msg body` is deliberately side-effect-free). The file outlives ack,
escalation, agent death, log retention, *and* restart → **monotonic disk
growth**, and trimming the referencing log strands the body as a guaranteed
orphan. **Why PROPOSE, not APPLY:** the clean fix touches 3 terminal paths
(`onAck`/`forgetInflight` + escalation + SessionEnd) and interacts with the
side-effect-free `body` read — a lifecycle change worth your eye. Self-verified
real (the finder's verifiers all failed; I confirmed by reading).

### 6. `delivery-0-0` — TTL/epoch skip-advance precedes the in-flight guard *(medium, correctness)*
In `dispatchAgentInbox`, TTL (`:570`) and epoch (`:583`) are evaluated *before*
the `in_flight_.contains(m.id)` guard (`:599`). An agent-inbox cursor only
advances on ACK, so an in-flight record sits at the head; if its TTL elapses
during the ACK wait, the loop advances the cursor *past a still-in-flight
record* and can dispatch a second one — double-dispatch + a cursor the monotonic
ACK guard can't reconcile. Narrow today (default mail `ttl_ms=0`), but any
shorter-than-ACK-timeout TTL reaches it. **Strong candidate**; PROPOSE because
it's a subtle ordering change in the hot dispatch path (and `dispatchTuiCommands`
has the identical structure). Fix: hoist the in-flight guard to the top of the
loop. Partial verify (1/0).

### 7. `delivery-1-0` — `load()` early-return makes `loadBlockingOps()` dead on every boot *(medium)*
`load()` returns at `:222` (`if (!fs::exists(inflightDir)) return;`) *before*
`loadBlockingOps()` at `:244`; `broker.cpp` wipes `in-flight/` right before
`load()`, so the early-return always fires and `loadBlockingOps()` never runs —
while `blocking-op/` files persist (not in the wipe set) and are never
reconciled. **Refuter's caveat (agreed):** impact is largely inert — the
pending-blocking-op gate re-derives state from the durable commands-log, so no
agent actually wedges; it's a leaked-file + dead-restore-code smell. PROPOSE
because the fix touches boot semantics. Verified 2/1.

### 8. `policy-1-0` — R1 idle-clear uses exponential relaunch-backoff, not the documented flat 5-min cooldown *(medium, latent)*
`recoverClear` gates the idle `/clear` through `recoveryRecord(...Clear...)`,
which arms `backoff_base_ms << attempts` and increments `attempts` unconditionally
(`recovery.cpp:154/157`); nothing resets `attempts` for the non-breaker Clear
signature. So idle-clear cadence drifts 30s→60s→…→30-min clamp, vs the
documented flat 5-min (`docs/broker-auto-recovery.md §5 R1`) and the legacy
`maybeAutoClear` (flat `5*60'000`, `delivery.cpp:1106`). **Latent:** only bites
when auto-recovery `soft`/`on` is activated (default is `observe`). Self-verified
real. Fix: flat cooldown for the Clear signature.

### 9. `rpc-1-0` — backpressure rejects only hit stderr, not the doc-promised audit record *(low, doc-vs-code drift)*
`docs/broker-intake-decouple.md §4` promises each backpressure reject "appends an
audit record (protocol=backpressure)"; the reject path (`rpc.cpp:451`) writes the
client error + one stderr line but **no audit record**. Genuine structural
tension: the audit log is processing-owned state that intake *cannot* touch from
the reject path (the §3/§6 no-shared-state invariant), so "intake appends audit"
can't be done as written. **Your call:** wire intake→atomic-counter→processing
appends, *or* amend the doc to record the stderr downgrade. Verified 2/1 (refuter:
client-error + broker-pane scrollback already surface it; only a redundant
durable copy is missing).

### 10. `policy-0-1` — breaker has no re-close edge wired *(low, Phase-C prerequisite)*
`recoveryObserveHealthy` (the only HalfOpen→Closed transition) has **zero
callers** (verified: def + decl + one comment, nothing in the actor/loop).
Mid-build-inert today because `Recover`/Relaunch is Phase-C-gated and a no-op
(`delivery.cpp:1298`). **Forward-looking:** when Phase C wires Recover, a tripped
breaker would never re-close even after the agent recovers. Flag so the Phase-C
wiring ships the re-close edge. Not a live bug.

---

## REFUTED / DOWNGRADED — surfaced for completeness, no action

### `rpc-2-0` — "shutdown drain races intake → fd leak + 5s hang" → **cosmetic**
The data-flow race is real (intake can `queue.push` after processing's final
drain), but the claimed harms are false: the stop path **terminates the process**,
so the OS closes the orphaned fd microseconds later (no leak, no EMFILE), and the
5s figure is the *broker's* `SO_RCVTIMEO` on accepted conns, not the client's —
the client gets a prompt EOF at broker exit, not a 5s hang. Residual is purely
cosmetic (a racing client gets a generic EOF instead of a "shutting down" JSON
reply on a dying process). Refuted as medium.

### `pane-0-0` — "hung zellij blocks shutdown up to 5s" → **by design**
The mechanics are accurate (the wait loops gate only on a deadline, swallow
EINTR, never check the stop flag), but `docs/deep/broker-internals-cpp.md §1.4`
documents this exact code: the bounded **5s SIGKILL** *is* the deliberate ceiling
for a wedged subprocess, and the design's improvement path is moving dump-screen
off the loop thread (a bigger redesign), not a stop-flag check. Worst case is a
one-time ≤5s shutdown delay degrading to a clean SIGKILL — categorically unlike
the unbounded `accept()` wedge (already fixed). By-design, bounded.

---

---

## Second review pass — additional findings (bonus)

A second run (a mode-routing bug re-ran `review` instead of `map`) sampled
differently and surfaced findings the first pass missed — including a HIGH the
first pass didn't see. Its synthesis stage *did* run (real LLM buckets). All
**PROPOSE** or REFUTED; none changes the APPLY-NOW set.

### 11. `kernel-0-0` — short write in `append()` leaves a torn record mid-log *(HIGH — self-verified, 3/0 unanimous)*
`topic_log.cpp:303` — `::write` on the O_APPEND fd can return a short count
(EINTR, ENOSPC mid-record, kernel split). The code returns an error, but the
**partial bytes are already durable**; because the fd is O_APPEND the *next*
append lands after them, so the torn record is no longer the tail. `parseFrom`'s
refuse-torn-*tail* invariant only guards EOF truncation — a partial record
*buried mid-file* is read as real: its first 8 bytes become a bogus `rec_len`
and **everything after it misframes**, collapsing byte-offset cursor addressing +
at-least-once for that topic. This is a **rule-#1 (append-log) violation**.
**Why PROPOSE not APPLY:** it's the kernel append path the whole broker rests on;
the fix (`fstat` pre-write size + `ftruncate` back on short write, *or* loop the
write handling EINTR) deserves a design-doc-first pass and the kernel owner, not
a live edit by me. **Strongly recommend prioritizing.**

### 12. `rpc-1-0` — uncapped blocking `flock(LOCK_EX)` on the processing thread *(hardening; claimed HIGH, refuted to nit)*
`dispatch.cpp:48` (and `delivery.cpp:118`) acquire the per-pane TTY lock with a
*blocking* `flock`, looping only on EINTR — no `LOCK_NB`, no timeout, no
stop-flag check — on the broker's single processing thread. The Phase-0 cap audit
only enumerated subprocess *forks*, not this blocking syscall. **Refuter (agreed):
unreachable in normal operation** — the only lock holders are timeout-capped
transient CLI processes (5s SIGKILL). Real as a **defensive-hardening target**
(`LOCK_NB` + bounded retry + `stopRequested()` check) and an audit-scope nit.

### 13. `pane-0-1` — `list-panes` cache poisons a *failed* fetch as empty for the full TTL *(medium, monitor-truth)*
`pane.cpp:480` stores `cached = rc==0 ? out : ""` but unconditionally marks the
cache valid. A timed-out/failed `list-panes` (rc=-1) caches **empty** for the TTL
(≤3s), so every `paneId()` returns `{}` → the *whole fleet* reads pane-less for
that window (flaps doorbell strand-tracking, skips token scans, trips GONE in the
monitor). **Refuter (agreed):** real one-line smell; impact transient +
self-healing + confined to 5s recovery scans (no mail lost). **Monitor-must-not-
lie class.** Fix: only a *successful* fetch refreshes the cache.

### 14. `cli-0-0` — `bus topic create --max-bytes N` documented but rejected *(medium)*
`docs/bus-commands.md:22` advertises `--max-bytes`, but the create flag loop
(`sub_topic.cpp:111-126`) rejects it (exit 2, topic not created). **Not a clean
apply:** `max_bytes` is *not* a `TopicConfig` field — it exists only as
`retention.h::planTrim`'s parameter, fed per-trim from a **global env override**,
not per-topic. Honoring the flag needs a new config field + handler + per-topic
read (a feature), *or* dropping the flag from the doc. **Your decision.**

### 15. `cli-0-1` — missing-value flags exit 2 silently, inconsistently *(low)*
~10 flag parsers (`sub_produce.cpp:86/89/92/...`, `sub_topic`, `sub_consume`)
do `if (++i >= args.size()) return 2;` with no message, while siblings
(`sub_events.cpp:116`, `done --id`) print a usage line. Cosmetic UX
inconsistency on a malformed-invocation path; a shared flag-parse helper is the
real fix (ties into the A8 DRY tension). Low priority.

### REFUTED in the second pass
- `rpc-0-2` (client `call()` has no read timeout) → **by design**: the asymmetry
  protects the *shared* intake thread, not the per-process client (documented in
  broker-hardening-batch). A hung daemon blocking its one-shot CLI is the standard
  local-daemon contract (Ctrl-C / retry).
- `rpc-0-0` (shutdown drain race) → duplicate of `rpc-2-0`; cosmetic.
- `policy-0-0`(2nd, positional batch consume) → the claimed dup+drop mechanism
  doesn't exist (appends/consumes run lockstep in emission order); the
  single-action sliver it gestures at is **already fixed** by APPLY-NOW #1. An
  id-addressed `consumeQueueHead(topic, expected_id)` is a nice follow-up, not
  required.

## Tooling gaps surfaced by this run (to harden `workflows/review.js`)

1. **`votes:0, refutes:0` must NOT count as confirmed.** When all verifiers
   fail to emit `StructuredOutput`, `verifyFindings` currently sets
   `survives:true` — an *unverified* finding masquerading as confirmed. Fix:
   require ≥1 real (non-null) verdict to survive; otherwise mark `unverified`
   and route to a re-verify or to PROPOSE-with-caveat.
2. **High verifier `StructuredOutput` miss rate.** Heavy-tool-use verifiers
   sometimes end on prose. Harden the prompt ("your FINAL action MUST be the
   StructuredOutput call") and/or add a one-shot re-ask tier.
3. **Floor can't bucket.** The deterministic floor dumps everything to PROPOSE.
   Acceptable (findings survive), but the APPLY-NOW/PROPOSE split then needs a
   human pass — which is what happened here. Consider a pure-JS heuristic
   pre-bucket (severity+confidence+lens) so the floor degrades less.
