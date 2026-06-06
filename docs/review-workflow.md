# review-workflow.md — reusable multi-agent review + comprehension harness

**Status:** PLAN — design-gated, awaiting auri ratify before the big fan-out.
**Task:** #1780722034328-chronicler-ff54 (supersedes the interrupted blind audit).
**Owner:** chronicler.

## Why this exists

sulin wants to drive a guided 1:1 review/redesign of claude-bus. He needs two
things from the fleet, both *reusable as the code evolves*, not one-off:

1. **A review harness** that fans finders out over the codebase by area × lens,
   adversarially verifies each finding (majority-refute kills), and synthesizes
   confirmed gaps + cleanups. Re-runnable on every future diff.
2. **Handoff artifacts** — a system map, per-area what/why/why-VALUABLE briefs,
   and a file reading-order — that let *him* direct the redesign to his vibe.
   These ENABLE his review; they are not a finished design.

Both share one durable thing: the **area map** (the decomposition of the repo
into review units). So this is ONE workflow script, two modes, one shared map.

## The area map (single source of truth)

The five CMake library boundaries already partition the code by authority and
dependency — they ARE the review units. Each area carries its files, the lens
bundle tuned to where failures bite, and the design doc(s) that define its
invariants (so a finder can tell "violates the design" from "is the design").

| # | Area | Files | LOC | Lens bundle | Invariant docs |
|---|------|-------|-----|-------------|----------------|
| A1 | Kernel substrate | topic_log, topic_registry, json_min, event | ~1.2k | correctness, concurrency, invariant | broker-spec, broker-seam-redesign |
| A2 | Delivery loop ⚠ | delivery.{cpp,h} | ~2.0k | ALL (split 2 finders: ack/cursor/in-flight vs doorbell/token/trim/escalate) | broker-spec, policy-actors |
| A3 | Broker daemon | broker, signals, state_paths, retention | ~1.2k | concurrency, invariant | broker-spec, broker-intake-decouple |
| A4 | RPC / wire ⚠ | rpc, dispatch | ~0.7k | correctness, concurrency | broker-intake-decouple |
| A5 | Policy actors ⚠ | policy, recovery_actor, dispatch_actor, blackboard_actor, recovery | ~0.8k | correctness, invariant | policy-actors, work-queue-dispatch, broker-auto-recovery |
| A6 | Readers / derivation | agent_status, task_model, trigger_feed, pane_state, tail_reader | ~1.3k | correctness, invariant | broker-seam-redesign, observability-viewers |
| A7 | Pane I/O | pane, tty_policy | ~0.9k | correctness, concurrency | off-TTY, safe-pane-write notes |
| A8 | CLI surface | sub/*.cpp, bus, bus_main | ~4.9k | correctness, simplification | bus-commands |

⚠ = the new code the superseded audit targeted; gets extra finders + a
loop-until-dry tail pass.

### Lenses

- **L1 correctness** — logic bugs, off-by-one, cursor/ack/epoch invariants, edge + error paths.
- **L2 concurrency / crash-safety** — races, partial/torn writes, fd + signal handling, suspend/clock-jump, write atomicity.
- **L3 resource safety** — fd leaks, unbounded per-agent map growth, the dead-agent GC story.
- **L4 invariant integrity** — does the code preserve the DOCUMENTED design? kernel triad (append-log + cursor-advances-on-ack-only + boot-epoch), Policy no-cursor-verb, link boundaries, escalate-once.
- **L5 simplification / dead-code** — vestigial code, over-abstraction, duplication. Feeds the cleanup deliverable; also runs as a cross-cutting sweep.

## Mode 1: `review` (critique + cleanup)

Pipeline, no barrier between stages (a finding verifies the moment its finder
returns):

```
find(area, lenses, docs)  →  verify×3 (distinct lenses, majority-refute kills)  →  synthesize(hardened)
```

**Find.** One finder per (area, lens-bundle); ~12 finders total (A2 gets 2, plus
a cross-cutting L5 sweep over all areas). Each finder reads its files + the cited
design doc and returns a `findings[]` schema: `{title, file, line, lens,
severity, claim, why_it_matters, suggested_fix, confidence}`. The ⚠ areas also
get a **loop-until-dry** tail (keep spawning finders until 2 consecutive dry
rounds) — the "blind audit" thoroughness lever.

**Verify (adversarial, the load-bearing guard).** Each finding draws 3 skeptics
with DISTINCT lenses, each prompted to refute:
- **V-correctness** — "reconstruct the concrete trigger; default refuted if you can't."
- **V-design-intent** — "read the cited doc + callers; is this a DELIBERATE escape-hatch / mid-build-inert / documented seam? if intended, refute." *(This is the explicit guard against the ~30%-false-positive blind-audit failure mode: verify-before-remove.)*
- **V-impact/repro** — "build the input sequence that triggers it; refute if negligible or unreachable."

≥2 refutes ⇒ killed (logged with why). Survivors carry a merged verdict + a
confidence tier.

**Synthesize (hardened, per docs/ultracode-synthesis-pattern.md).** Schema every
stage, pure-JS dedup/bucketing as the floor, opus→sonnet→haiku model ladder,
findings ALWAYS reach the `return` (the one durable channel). Output buckets:
- **APPLY-NOW** — confirmed, high-confidence, low-risk, surgical → I apply.
- **PROPOSE** — confirmed but higher-risk or design-touching → report to sulin/auri, do NOT auto-apply (respects "enable his review, not a finished design").
- **REFUTED** — killed, with the refutation, for the audit trail.

### Applying cleanup (deliverable 2)

APPLY-NOW items applied **sequentially in my chronicler workspace** (single
writer = no conflict). jj workspaces (NOT git-worktree) only if the set is large
+ independent enough to parallelize. After each batch: `cmake --build` +
`ctest` green before anything lands. Nothing design-touching auto-applies.

## Mode 2: `map` (comprehension / handoff)

Lighter fan-out: one explainer per area (8) → one synthesis. Each explainer
reads its area + LEVERAGES existing docs (cites, doesn't re-derive; flags where
doc and code diverge) and returns: `{what, why_shaped_this_way, why_valuable
(what regresses without it), entry_points, intra_area_reading_order,
design_tensions}`. Synthesis assembles three artifacts:
- **SYSTEM MAP** — the 8 areas, the library DAG, the data flow (events.jsonl → readers → policy → delivery → pane), the kernel triad.
- **Per-area briefs** — what / why / why-valuable, surfacing tensions + alternatives-considered, NOT prescribing.
- **File READING-ORDER** — a guided path: kernel → readers → policy → delivery → broker/rpc → pane → CLI.

Builds on prior dissection-explainer runs; this refreshes + structures them for
a live walkthrough.

## Decisions I'm making (flag if you disagree)

1. **One script, two modes, shared area map** — over two separate scripts. The
   area map is the durable reusable thing; both modes consume it; sulin edits
   the map + re-runs as code evolves.
2. **Auto-apply only APPLY-NOW (high-confidence + low-risk + surgical).**
   Everything design-touching → PROPOSE bucket, sulin's call. Keeps me from
   pre-empting the redesign he wants to drive.
3. **Model tuning** (max-throughput): opus finders (depth), sonnet verifiers
   (high fan-out, refute-or-not is cheap), opus synthesis. Both levers, not
   throttled.

## Open question for auri

- Run order: **review mode first** (cleaner code for him to read) **then map
  mode** on the cleaned tree — or map first so the handoff reflects today's
  code exactly? I lean review→map. Your call.

## Rails

Design-gated (this doc → auri before fan-out) · jj workspaces not git-worktree ·
verify-before-remove is load-bearing · surgical changes, build+ctest green
before land · findings carry file:line + the invariant checked against ·
nothing design-touching auto-applies.
