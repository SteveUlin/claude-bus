#include "harness.h"
#include "task_model.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace bus;

namespace {

auto mk(std::string id, std::string_view state_str,
        std::vector<std::string> deps = {}) -> Task {
  Task t;
  t.id = std::move(id);
  t.title = t.id;  // title == id keeps assertions readable
  t.state = taskStateFromStr(state_str);
  t.deps = std::move(deps);
  return t;
}

auto has(const std::vector<std::string>& v, std::string_view id) -> bool {
  return std::ranges::find(v, id) != v.end();
}

}  // namespace

// ─── longest chain (structural critical-path proxy) ───

TEST(graph_longest_chain_orders_root_to_leaf) {
  // c depends on b depends on a → chain a → b → c (root → leaf).
  std::vector<Task> tasks{
      mk("c", "open", {"b"}),
      mk("b", "in_flight", {"a"}),
      mk("a", "done"),
  };
  const auto g = buildTaskGraph(tasks);
  CHECK_EQ(g.longest_chain.size(), std::size_t{3});
  CHECK_EQ(g.longest_chain[0], std::string{"a"});
  CHECK_EQ(g.longest_chain[1], std::string{"b"});
  CHECK_EQ(g.longest_chain[2], std::string{"c"});
  CHECK(!g.has_cycle);
}

TEST(graph_no_deps_no_chain) {
  std::vector<Task> tasks{mk("x", "open"), mk("y", "done")};
  const auto g = buildTaskGraph(tasks);
  CHECK(g.longest_chain.empty());  // depth 0 everywhere → no chain
}

// ─── ready vs blocked ───

TEST(graph_ready_is_open_with_deps_done) {
  // x (open) depends only on a (done) → ready. y (open) has no deps → ready.
  std::vector<Task> tasks{
      mk("a", "done"),
      mk("x", "open", {"a"}),
      mk("y", "open"),
  };
  const auto g = buildTaskGraph(tasks);
  CHECK(has(g.ready, "x"));
  CHECK(has(g.ready, "y"));
  CHECK(g.blocked.empty());
}

TEST(graph_blocked_when_a_dep_not_done) {
  // x (open) depends on b (in_flight, not done) → blocked, not ready.
  std::vector<Task> tasks{
      mk("b", "in_flight"),
      mk("x", "open", {"b"}),
  };
  const auto g = buildTaskGraph(tasks);
  CHECK(has(g.blocked, "x"));
  CHECK(!has(g.ready, "x"));
}

TEST(graph_in_flight_with_deps_met_is_neither) {
  // An in_flight task with satisfied deps is already running — not surfaced
  // as ready (can't "start" it) nor blocked.
  std::vector<Task> tasks{
      mk("a", "done"),
      mk("r", "in_flight", {"a"}),
  };
  const auto g = buildTaskGraph(tasks);
  CHECK(!has(g.ready, "r"));
  CHECK(!has(g.blocked, "r"));
}

TEST(graph_done_task_never_ready_or_blocked) {
  std::vector<Task> tasks{mk("d", "done", {"missing"})};
  const auto g = buildTaskGraph(tasks);
  CHECK(g.ready.empty());
  CHECK(g.blocked.empty());
}

// ─── integrity: dangling deps + cycles ───

TEST(graph_flags_dangling_dep) {
  std::vector<Task> tasks{mk("x", "open", {"ghost"})};
  const auto g = buildTaskGraph(tasks);
  CHECK_EQ(g.dangling.size(), std::size_t{1});
  CHECK_EQ(g.dangling[0].task_id, std::string{"x"});
  CHECK_EQ(g.dangling[0].missing_id, std::string{"ghost"});
  // A dangling dep can't be "done" → the task is blocked, not ready.
  CHECK(has(g.blocked, "x"));
}

TEST(graph_detects_cycle_and_terminates) {
  // a → b → a. Must flag the cycle and not infinite-loop.
  std::vector<Task> tasks{
      mk("a", "open", {"b"}),
      mk("b", "open", {"a"}),
  };
  const auto g = buildTaskGraph(tasks);
  CHECK(g.has_cycle);
}

// ─── taskStateFromStr / taskStateToStr roundtrip ───

TEST(task_state_roundtrip_all_values) {
  // Every known wire string survives a fromStr → toStr roundtrip unchanged.
  for (std::string_view s : {"open", "in_flight", "done", "cancelled"}) {
    CHECK_EQ(taskStateToStr(taskStateFromStr(s)), s);
  }
  // Unknown string → Unknown enum → "unknown" wire string.
  CHECK_EQ(taskStateFromStr("bogus"), TaskState::Unknown);
  CHECK_EQ(taskStateToStr(TaskState::Unknown), std::string_view{"unknown"});
}
