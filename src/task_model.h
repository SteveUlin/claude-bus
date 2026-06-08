// The fleet task model — a single read model over the existing stores.
//
// CONVERGENCE, not a new store (auri's constraint): a Task joins what the
// fleet already records. The live ground truth (see docs/task-model.md):
//   - $STATE/done/<agent>.jsonl  — the only durable per-task record, and
//     TERMINAL-ONLY (a task appears here only once finished).
//   - $STATE/triggers/<agent>.json — per-AGENT liveness, joined as an
//     owner overlay (who's at a safe boundary, how full).
//   - broker work-queue — the dormant capability that WOULD hold open /
//     in-flight tasks once dispatch mints identity (Option B). Until then
//     this model is terminal-only (Option A) and degrades gracefully:
//     the viewer renders whatever subset of {open,in_flight,done} exists.
//
// Pure reader — no broker round-trip, no writes. Mirrors trigger_feed:
// shared derivation, two surfaces (the `bus tasks` CLI + the critical-path
// view to come). See docs/task-model.md.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "trigger_feed.h"

namespace bus {

enum class TaskState { Open, InFlight, Done, Cancelled, Unknown };
auto taskStateToStr(TaskState) -> std::string_view;
auto taskStateFromStr(std::string_view) -> TaskState;

// The open-task spine (task-model B): an append-log topic that holds the
// task records minted at dispatch. The producer (`bus tasks open/close`)
// writes via the broker enqueue RPC; the reader projects over it via
// Journal::dump() (read-in-full, no cursor). Shared by both sides.
inline constexpr std::string_view kTasksTopic = "tasks";

// Owner-liveness overlay, joined from $STATE/triggers/<agent>.json. `valid`
// is false when the trigger file is absent or stale (computed_at_ms too old
// — the writer is a viewer and viewers die; a stale overlay is no-overlay).
struct OwnerLive {
  bool valid{false};
  Boundary boundary{Boundary::None};  // Done | Idle | None
  int ctx_pct{-1};
};

// A completion claim from $STATE/done. ts < 0 means "no claim" (an open /
// in-flight task under Option B); under Option A every task has one.
struct DoneClaim {
  std::string artifact;
  std::int64_t ts{-1};
};

struct Task {
  std::string id;     // Option A: synth "<owner>-<ts>"; Option B: dispatch-minted
  std::string owner;  // agent name
  std::string title;  // human-readable (done.task, or a dispatch title under B)
  TaskState state{TaskState::Unknown};
  std::vector<std::string> deps;  // task ids; empty until B exists
  DoneClaim done_claim;           // ts<0 => none
  OwnerLive owner_live;           // valid=false => none
};

// Build the task model from the live stores, newest-first. `only` filters
// to a set of owners when non-empty. Pure: reads $STATE/done (+ $STATE/
// triggers overlay) read-only.
auto readTasks(const std::set<std::string>& only) -> std::vector<Task>;

// ── critical-path / dependency graph (the structural half) ──
// Derived purely from Task.deps — the slowest-chain *structure*. Edge weight
// is hop count until elodin's span durations land (then weight = duration and
// the same longest-path query becomes the true critical path). See
// docs/critical-path.md.

// A dep edge that points at a task id with no matching task (typo, or a
// spine record the broker GC'd). Surfaced as a data-integrity warning.
struct DanglingDep {
  std::string task_id;     // the task that declared the dep
  std::string missing_id;  // the dep id with no matching task
};

struct TaskGraph {
  // OPEN tasks whose deps are all done (or which have none) — not yet
  // started + actionable. (An in_flight task with met deps is already
  // running, so it's neither ready nor blocked.) Newest-first.
  std::vector<std::string> ready;
  // Open OR in_flight tasks with at least one not-done dep. Newest-first.
  std::vector<std::string> blocked;
  // Longest dependency chain by hop count, ordered root → leaf (a dep comes
  // before the task that needs it). The structural critical-path proxy.
  std::vector<std::string> longest_chain;
  std::vector<DanglingDep> dangling;
  bool has_cycle{false};  // deps loop — a dispatch mistake; chain is then
                          // best-effort (the cycle is excluded from depth).
};

// Pure graph derivation over a task set (typically readTasks's output).
// Self-contained: no I/O. Unit-tested in tests/unit.
auto buildTaskGraph(const std::vector<Task>& tasks) -> TaskGraph;

}  // namespace bus
