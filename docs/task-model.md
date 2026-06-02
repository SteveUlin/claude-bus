# task-model — fleet task convergence (kvothe lane)

Author: kvothe · 2026-06-01 · For: auri's observability pillar (task-tracking + critical-path)
Status: **BUILT (Option B) — open-task spine live, verified end-to-end.**
auri ruled all 5 design decisions (append-log spine; in_flight INFERRED from
owner liveness; id = `<ms>-<owner>-<rand4>`, minted dispatch-side; deps
advisory; standalone `bus tasks open`). Implemented + sandbox-verified:
`bus tasks open/close` (producers via the enqueue RPC), `bus done --id`
(optional `task_id`, purely additive), and `readTasks` reconcile over THREE
stores — the `tasks` append-log spine (open/cancelled) + `$STATE/done`
(terminal, by `task_id`) + the triggers liveness overlay (in_flight
inference). `bus tasks` renders OWNER/STATE/TITLE/DEPS/DONE/LIVE with state
colors. **No broker changes** (elodin untouched — producers use the existing
enqueue op; the reader reads the log via TopicLog::dump()). Round-trip proven:
open→open, `done --id`→done, `close`→cancelled, liveness none→in_flight,
legacy `bus done` (no id)→terminal, owner filter, sort by urgency-then-recency.
Critical-path view (next) joins tasks↔elodin's span schema.

Durable anchor (survives /clear). Builds on
[docs/p3-trigger-feed.md](p3-trigger-feed.md) and
[docs/observability-pillar.md](observability-pillar.md).

## Goal

A single read model — one task = `{id, owner, state, deps, done_claim}` —
so a viewer can answer "what is the fleet doing, what's blocked on what,
and where's the critical path." The headline consumer is the critical-path
view (next lane).

auri's framing: **convergence, not greenfield. Unify the existing stores,
don't invent a 4th.**

## Ground truth (what the stores ACTUALLY are — live, 2026-06-01)

I inspected the live `$STATE` before designing. The "3 stores" are NOT three
symmetric task stores:

| Store | What it really holds | Task-level? |
| --- | --- | --- |
| `$STATE/done/<agent>.jsonl` | `{agent, task, claimed_artifact, ts}` completion claims | **Terminal only** — a task shows up here *only once finished*. The one durable per-task record. |
| `$STATE/triggers/<agent>.json` | per-**agent** `{urgency, safety, computed_at_ms}` | No — owner *liveness* overlay, not tasks. |
| broker `work-queue` | kind is implemented; **zero instances registered** | Dormant capability, not a populated store. |

Other dirs checked, none a task store: `agents/` = registry,
`blocking-op/` = coordination locks, `in-flight/` = broker per-message
delivery (empty), `audit`/`inbox-*`/`commands-*` = message topics.

**The gap that defines the design:** there is **no store of open /
in-flight tasks**. Open work lives implicitly in auri's dispatch mail
(`inbox-<agent>` records + the human-readable `--title`) and in auri's own
coordination. `done` gives the *closed* set; nothing gives the *open* set
with identity + deps.

## The identity decision (SURFACE, don't pick silently)

A task-model needs a stable `id` and `deps`. Where do they come from?
Three readings of "unify the 3 stores," in increasing cost:

- **A · Terminal-only (done-derived).** Build the model purely from `done`
  stamps. `id` = synthesized from `(agent, task, ts)`; `state` always
  `done`; `deps` = none. **Cost: ~nil** (pure reader, my lane entirely).
  **Limit: no open/in-flight visibility** — you only ever see finished
  work. A "critical path" over only-completed tasks is a post-mortem, not
  a live cockpit. Useful as a stepping stone; weak as the headline.

- **B · Activate work-queue as the open-task spine (auri's literal "3
  stores, no 4th").** Tasks get identity at *dispatch*: enqueue a task
  record `{id, owner, deps, title}` to a `work-queue` topic when auri
  assigns work; `done` closes it (match on `id`); `triggers` overlays
  owner liveness. This uses exactly the 3 named stores and invents no 4th.
  **Cost: a dispatch-side PRODUCER must exist** — auri/comms must enqueue
  task records (and carry `id` through to the `bus done` stamp). That's a
  **dispatch-behavior change in auri/comms's lane, not mine.** I own the
  read model + viewer; I cannot unilaterally make tasks have identity.

- **C · Infer identity from dispatch mail (no behavior change).** Parse
  `inbox-*` titles + `done` tasks and fuzzy-match open↔closed. **Cost:
  brittle** (string matching, no real `id`/`deps`), and it reads message
  topics as a task store they weren't designed to be. Rejected unless A+B
  are both blocked.

**My recommendation: A now, B as the target.** Ship the terminal-only read
model + schema immediately (pure reader, unblocks the critical-path view's
plumbing against real data), and design the schema so adding the open-task
spine (B) is additive — the viewer renders whatever subset of
`{open, in-flight, done}` the stores can supply. Then the open question for
**auri** is purely: *do we want task identity minted at dispatch?* If yes,
that's a small producer in her/comms's lane (enqueue `{id,...}` on assign,
echo `id` into `bus done`), and B lights up with no viewer rework.

## Proposed read schema (degrades gracefully)

```
Task {
  id:         string   // dispatch-minted (B) | synth from done (A)
  owner:      string   // agent name
  title:      string   // human-readable (from done.task or dispatch title)
  state:      "open" | "in_flight" | "done" | "unknown"
  deps:       string[] // task ids; empty until B exists
  done_claim: { artifact, ts } | null   // from $STATE/done
  owner_live: { boundary, ctx_pct } | null  // overlay from $STATE/triggers
}
```

- `state` is computed: `done` if a matching done-claim exists; else
  `in_flight` if the owner's trigger shows boundary=none (mid-turn on it);
  else `open`. Under A-only, everything resolves to `done` (or `unknown`).
- `owner_live` joins the trigger feed so the same view shows *who's at a
  safe boundary* — ties task-model to the P3 work already shipped.

## Build order (once identity decision lands)

1. **Reader lib** `src/task_model.{h,cpp}` — pure: read `done/*` (+ `triggers/*`
   overlay), emit `Task[]`. Mirrors `trigger_feed.{h,cpp}` (shared
   derivation, no broker round-trip). Unit-testable like `bus_core`.
2. **`bus tasks` CLI** — `ID | OWNER | STATE | DEPS | DONE` table, the
   glance + the same shape the critical-path view consumes.
3. **Critical-path view** (next lane) consumes `Task[]` + elodin's span
   schema to trace the slowest dependent chain.

## Cross-lane routing (who owns what)

- **kvothe (me):** the read model, `bus tasks`, the views. Pure consumer.
- **auri:** the identity decision (A vs B), and if B — owns/assigns the
  dispatch-side task-record producer. This is the blocking input.
- **comms/auri:** if B, threading `id` through dispatch + `bus done`.
- **elodin:** span schema (already handed the CORE) — the critical-path
  view joins tasks↔spans, not task-model itself.

## B — recording verb + open-task spine (DESIGN; surface before impl)

auri APPROVED A-now-B-target and committed to the minting side. B gives
tasks IDENTITY at dispatch so the model shows open / in_flight, not just
done. Split: **auri mints `{id, owner, deps, title}` at dispatch; I build the
recording verb + the spine + the reconcile.**

### The convergence (still no 4th store)

B activates the dormant 3rd store — the work-queue topic — as the OPEN-task
registry. The reader then joins THREE existing stores by `id`:

| Source | Role in B | Mechanism |
| --- | --- | --- |
| work-queue topic `tasks` | open-task registry: `{id, owner, deps, title}` minted at dispatch | `TopicLog::dump()` reads ALL records non-destructively (no cursor advance) — same read-in-full pattern A uses on `done/*.jsonl`. |
| `$STATE/done/<agent>.jsonl` | terminal claim, linked by `task_id` | `bus done --id <id>` echoes the id so the reader matches open→done. |
| `$STATE/triggers/<agent>.json` | in_flight inference (owner mid-turn on it) | existing owner-liveness overlay (A already joins this). |

`state` precedence per id: a matching done-claim ⇒ `done`; else a `cancelled`
record ⇒ `cancelled`; else owner liveness `boundary=none` ⇒ `in_flight`;
else `open`.

### Recording verbs (the design to approve)

- **`bus tasks open --id ID --owner NAME --title "…" [--deps a,b]`** — thin
  wrapper that builds the task JSON and enqueues it to the `tasks` topic
  (auto-create on first use). Co-located under `bus tasks` (the task-model
  surface) rather than the existing `bus task` (which is the unrelated
  monitor-column setter — overloading it would conflate two concepts).
  Mechanically this is `bus msg enqueue tasks '<json>'`; the verb is sugar
  so auri's dispatch never hand-builds JSON.
- **`bus done --id ID "<task>" "<artifact>"`** — `bus done` gains an OPTIONAL
  `--id`; the done record gains `task_id`. Omitting it keeps today's
  A-style terminal-only stamp (B is purely additive — old calls still work).
- **`bus tasks close --id ID`** — append a `cancelled` record so an
  abandoned/dispatched-but-dropped task leaves the open list (else it grows
  unbounded). Lightweight; the reader folds it.

No broker changes: enqueue uses the existing produce path; the reader reads
the topic log file via `TopicLog::dump()`. Stays in my lane + auri's mint —
**elodin untouched.**

### Decisions for auri — RULED (2026-06-02)

1. **Topic kind = append-log.** work-queue's cursor/fetch is dead weight
   when we read-in-full and auri push-assigns `owner` at mint. append-log
   is the honest kind for an immutable event log we project over.
2. **in_flight = INFERRED** from owner liveness. Zero agent burden. The
   approximation, documented: `in_flight` means "the owner is live", NOT
   "provably on THIS task". Explicit `bus tasks start` deferred to later
   only if the inference misleads (an agent juggling two tasks).
3. **id = `<ms>-<owner>-<rand4>`** — mirrors the broker msg_id format
   (sortable, consistent). auri mints; the verb takes it opaque.
4. **deps = advisory** in v1 — recorded for the critical-path graph, NOT
   gating (auri is the scheduler; deps don't block dispatch).
5. **verb = standalone `bus tasks open`** — no folding into mail in v1.
   auri opens champion-less tasks with no mail to send, so task-creation
   must not couple to the mail path.

### Build order — DONE

1. ~~`bus done --id` + `task_id` in the done record~~ — done
   (`sub_produce.cpp`; optional flag, additive).
2. ~~`bus tasks open` / `close` verbs~~ — done (`sub_tasks.cpp`; sugar
   over the enqueue RPC, idempotent topic_create).
3. ~~`readTasks` reconcile over the `tasks` spine~~ — done
   (`task_model.cpp`; dump() the log, join `done` by `task_id`, state
   precedence, liveness overlay, urgency-then-recency sort).
4. ~~`bus tasks` viewer: DEPS column + richer states~~ — done
   (OWNER/STATE/TITLE/DEPS/DONE/LIVE, state-colored).

## Open / next (A + B — DONE)

1. ~~auri: A-now-B-target?~~ — A landed; B approved + ruled + BUILT.
2. ~~reader lib + `bus tasks` (A scope)~~ — DONE (terminal-only).
3. ~~B: recording verbs + open-task spine + reconcile~~ — DONE, verified.
4. NEXT: **dispatch-side adoption** — auri mints ids + calls `bus tasks
   open` at dispatch (her lane); agents echo `--id` into `bus done`. Until
   adoption, the spine is empty and `bus tasks` shows the legacy
   terminal-only view (graceful). Then: **critical-path view** joins
   `Task.deps` ↔ elodin's span schema for the slowest dependent chain.
