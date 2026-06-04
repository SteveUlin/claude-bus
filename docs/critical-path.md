# critical-path — the slowest dependent chain over the task DAG (kvothe lane)

Author: kvothe · 2026-06-03 · For: auri's observability pillar (the headline
consumer of task-model B's deps). Status: **DESIGN + v1 deps-graph (the view
LEADS the data — structure now, span-durations when elodin's schema lands).**
Builds on [[project_task_model_b]] (`Task.deps`) and the span schema to come.

Durable anchor (survives /clear).

## Goal

Trace the **critical path** — the dependency chain that gates fleet
completion. Two inputs, landing at different times:

1. **Structure** — the task DAG, from `Task.deps` (task-model B). **Available
   NOW.** Gives blocked-vs-ready, chains, cycle/dangling validation, and the
   longest chain *by hop count* (a structural critical-path proxy).
2. **Durations** — per-task elapsed time, from elodin's span schema (post-P2,
   not yet landed). Weights the nodes so "longest chain" becomes
   *longest-TIME* chain — the true critical path.

auri's framing: **the view can lead the data.** Build the structural half on
B's deps now; graft the span-weighting in when spans exist, no view rework.

## v1 — the deps-graph (buildable on `Task.deps` today)

A new reader over `readTasks()` (no new store; B already supplies `deps`):

- **Build the DAG:** `id → Task`, `id → deps[]`. Edges are advisory (auri's
  ruling) — recorded for THIS graph, never gating dispatch.
- **Validate (data integrity surfaces here):**
  - *dangling dep* — a `deps` id with no matching task → flag (typo, or a
    GC'd spine record).
  - *cycle* — deps that loop → flag (a dispatch mistake; would break any
    topological order).
- **Classify each open/in_flight task by dependency readiness:**
  - *ready* — no deps, or every dep is `done` → actionable, can start now.
  - *blocked* — at least one dep not yet `done` (annotate which, + its
    state). A `cancelled` dep is surfaced specially ("prereq abandoned —
    needs a decision"), not silently treated as satisfied.
- **Longest chain (structural critical-path proxy):** longest path by hop
  count through the DAG (DFS + memo over the topological order). This is the
  chain to watch; span-weighting later reorders it by time.

### Render (proposed — `bus tasks graph`)

Co-located under the `bus tasks` family (same surface as the viewer that
replaced the deck). One-shot + a `watch` mode later if it earns a pane.

```
TASK DAG  (deps advisory · durations pending spans)

critical chain (structural, 3 hops):
  ✓ design-spine → ◐ build-verb → ○ wire-viewer   [blocked on build-verb]

ready (deps satisfied):
  ○ docs-pass   ○ mint-helper
blocked:
  ○ wire-viewer    ⊣ build-verb (in_flight)
  ○ deploy         ⊣ test-suite (open), wire-viewer (open)
⚠ dangling: "deploy" → dep "missing-id" (no such task)
```

(✓ done · ◐ in_flight · ○ open/ready · ⊣ "blocked by"; titles, not raw ids,
where a title exists.)

## v2 — span-weighting (when elodin's schema lands)

Replace hop-count with summed/maxed span DURATION per node; the longest-TIME
path is the true critical path. The join key is the task `id` (spans carry it
if elodin stamps task_id, else owner+window overlap — coordinate the join key
with him when the schema lands). No view rework — only the edge weight
changes from `1` to `duration`.

## Scope / routing

- **kvothe (me):** the graph reader + `bus tasks graph` view + the
  structural metrics. v1 now; v2 weighting when spans exist.
- **elodin:** the span schema + a task_id stamp on spans (the join key).
  Coordinate the key when his schema lands (post-P2).
- **auri:** dispatch threads REAL cross-task `--deps` as they arise (not
  manufactured) → the graph fills with genuine edges over time.

## Status / next

1. ~~design~~ — DONE (this doc).
2. v1 deps-graph reader + `bus tasks graph` view — IN PROGRESS.
3. surface the first cut to auri; iterate on render.
4. v2 span-weighting — deferred to elodin's schema.
