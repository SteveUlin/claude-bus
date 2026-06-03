// `bus tasks` — the fleet task model: a viewer + the open-task recording
// verbs (task-model B).
//
// CONVERGENCE over the existing stores (no 4th store): joins the durable
// $STATE/done completion claims with the `tasks` append-log spine (open /
// in_flight) and the $STATE/triggers owner-liveness overlay. The reader
// derivation lives in task_model.{h,cpp}, shared with the critical-path
// view to come. See docs/task-model.md.
//
// Verbs:
//   bus tasks                       — render the model (default)
//   bus tasks [OWNER...]            — render, filtered to owners
//   bus tasks open --id ID --owner NAME --title "..." [--deps a,b]
//   bus tasks close --id ID
//
// open/close are thin sugar over the broker enqueue RPC — they write task
// records to the `tasks` append-log topic (the broker is the only writer to
// topic logs). No broker code changes; the reader reads the log file
// directly via TopicLog::dump().

#include "../agent_status.h"
#include "../broker.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../signals.h"
#include "../sub.h"
#include "../task_model.h"

#include <chrono>
#include <csignal>
#include <format>
#include <map>
#include <print>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopTasks = 0;
auto onSignalTasks(int) -> void { gStopTasks = 1; }

auto senderFromEnv() -> std::string {
  if (const char* env = std::getenv("CLAUDE_BUS_AGENT_ID")) return env;
  return "unknown";
}

auto fmtAge(std::int64_t ms) -> std::string {
  if (ms < 0) return "—";
  const auto s = ms / 1000;
  if (s < 60) return std::to_string(s) + "s";
  if (s < 3600) return std::to_string(s / 60) + "m";
  if (s < 86400) return std::to_string(s / 3600) + "h";
  return std::to_string(s / 86400) + "d";
}

auto truncate(std::string s, std::size_t w) -> std::string {
  if (s.size() <= w) return s;
  s.resize(w - 1);
  s += "…";
  return s;
}

// State-cell color — the glance signal. open=cyan (waiting), in_flight=
// yellow (live), done=dim green, cancelled=dim.
auto stateColor(std::string_view st) -> std::string_view {
  if (st == "in_flight") return kYellow;
  if (st == "open") return kCyan;
  if (st == "done") return kGreen;
  return kDim;  // cancelled / unknown
}

// Owner-liveness cell: boundary + ctx%, dimmed when no fresh overlay. Ties
// the task view to the P3 trigger feed — same glance shows who's free.
auto liveCell(const OwnerLive& l) -> std::string {
  if (!l.valid) return std::string{kDim} + "—" + std::string{kReset};
  std::string out{l.boundary};
  if (l.ctx_pct >= 0) out += " " + std::to_string(l.ctx_pct) + "%";
  return out;
}

auto depsCell(const std::vector<std::string>& deps) -> std::string {
  if (deps.empty()) return "—";
  std::string out;
  for (const auto& d : deps) {
    if (!out.empty()) out += ",";
    out += d;
  }
  return out;
}

// Split a comma-separated list, trimming surrounding whitespace.
auto splitCsv(std::string_view s) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',') {
      if (!cur.empty()) out.push_back(std::move(cur));
      cur.clear();
    } else if (c != ' ' && c != '\t') {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

// Ensure the `tasks` append-log topic exists, then enqueue one record
// body. Both go through the broker (the only writer to topic logs). The
// topic_create is idempotent: an "already exists" error is expected and
// benign on every call after the first.
auto enqueueTaskRecord(const std::string& body) -> int {
  const auto cfg = resolveConfig();

  {
    std::map<std::string, json::Value> mk;
    mk.insert({"op", json::Value::from("topic_create")});
    mk.insert({"name", json::Value::from(std::string{kTasksTopic})});
    mk.insert({"kind", json::Value::from(std::string{"append-log"})});
    // Roomy per-record cap: task JSON is small, but titles are free text.
    mk.insert({"max_record_bytes", json::Value::from(static_cast<std::int64_t>(8192))});
    auto r = rpc::call(cfg.socket_path, json::Value::fromObject(std::move(mk)));
    // Ignore the result: success on first call, "already exists" after.
    // A genuine broker-down failure surfaces on the enqueue below.
    (void)r;
  }

  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("enqueue")});
  req.insert({"topic", json::Value::from(std::string{kTasksTopic})});
  req.insert({"body", json::Value::from(body)});
  req.insert({"sender", json::Value::from(senderFromEnv())});
  req.insert({"protocol", json::Value::from(std::string{"task"})});
  req.insert({"deliver_when", json::Value::from(std::string{"immediate"})});
  auto resp = rpc::call(cfg.socket_path, json::Value::fromObject(std::move(req)));
  if (!resp) {
    std::println(stderr, "bus tasks: {}", resp.error().message);
    return 1;
  }
  if (!resp->getOrBool("ok")) {
    std::println(stderr, "bus tasks: {}", resp->getOrString("error"));
    return 1;
  }
  return 0;
}

// `bus tasks mint-id --owner NAME` — print a fresh task id in the broker's
// msg-id format `<ms:013>-<owner>-<rand:04x>` (sortable + consistent, auri's
// ruling). The dispatcher mints once, then threads it through `bus tasks
// open --id` and the recipient's `bus done --id`.
auto tasksMintId(std::span<const char* const> args) -> int {
  std::string owner;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--owner") {
      if (++i >= args.size()) return 2;
      owner = args[i];
    } else {
      std::println(stderr, "bus tasks mint-id: unknown flag \"{}\"", a);
      return 2;
    }
  }
  if (owner.empty()) {
    std::println(stderr, "usage: bus tasks mint-id --owner NAME");
    return 2;
  }
  thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<unsigned> dist{0, 0xFFFF};
  std::println("{:013}-{}-{:04x}", nowMs(), owner, dist(rng));
  return 0;
}

// `bus tasks open --id ID --owner NAME --title "..." [--deps a,b]`
auto tasksOpen(std::span<const char* const> args) -> int {
  std::string id, owner, title, deps_csv;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    auto next = [&](std::string& dst) -> bool {
      if (++i >= args.size()) return false;
      dst = args[i];
      return true;
    };
    if (a == "--id") {
      if (!next(id)) return 2;
    } else if (a == "--owner") {
      if (!next(owner)) return 2;
    } else if (a == "--title") {
      if (!next(title)) return 2;
    } else if (a == "--deps") {
      if (!next(deps_csv)) return 2;
    } else {
      std::println(stderr, "bus tasks open: unknown flag \"{}\"", a);
      return 2;
    }
  }
  if (id.empty() || owner.empty() || title.empty()) {
    std::println(stderr,
                 "usage: bus tasks open --id ID --owner NAME --title \"...\" "
                 "[--deps a,b]");
    return 2;
  }

  std::map<std::string, json::Value> rec;
  rec.insert({"id", json::Value::from(id)});
  rec.insert({"owner", json::Value::from(owner)});
  rec.insert({"title", json::Value::from(title)});
  std::vector<json::Value> deps;
  for (auto& d : splitCsv(deps_csv)) deps.push_back(json::Value::from(d));
  rec.insert({"deps", json::Value::fromArray(std::move(deps))});
  rec.insert({"ts", json::Value::from(nowMs())});
  const std::string body =
      json::serialize(json::Value::fromObject(std::move(rec)));

  const int rc = enqueueTaskRecord(body);
  if (rc == 0) std::println("task open: {} → {}", id, owner);
  return rc;
}

// `bus tasks close --id ID` — append a cancellation so an abandoned /
// dispatched-but-dropped task leaves the open list. The reader folds it
// (state precedence: a done-claim still wins over a cancel).
auto tasksClose(std::span<const char* const> args) -> int {
  std::string id;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--id") {
      if (++i >= args.size()) return 2;
      id = args[i];
    } else {
      std::println(stderr, "bus tasks close: unknown flag \"{}\"", a);
      return 2;
    }
  }
  if (id.empty()) {
    std::println(stderr, "usage: bus tasks close --id ID");
    return 2;
  }

  std::map<std::string, json::Value> rec;
  rec.insert({"id", json::Value::from(id)});
  rec.insert({"cancelled", json::Value::from(true)});
  rec.insert({"ts", json::Value::from(nowMs())});
  const std::string body =
      json::serialize(json::Value::fromObject(std::move(rec)));

  const int rc = enqueueTaskRecord(body);
  if (rc == 0) std::println("task close: {}", id);
  return rc;
}

// Emit the task table. `live` = continuous-refresh framing: each line gets
// a clear-to-EOL suffix and the block is positioned by the caller (cursor
// home + clear-below) so a redraw never leaves stale trailing glyphs. The
// one-shot path passes live=false for plain stdout.
auto emitTable(const std::set<std::string>& only, bool live) -> void {
  const auto eol = live ? kClearEol : std::string_view{""};
  const auto tasks = readTasks(only);
  if (tasks.empty()) {
    std::println("no tasks yet — dispatch mints them with "
                 "`bus tasks open`, agents close with `bus done`{}", eol);
    return;
  }
  const std::int64_t now = nowMs();
  std::println("{}{:<10} {:<10} {:<34} {:<10} {:<7} {}{}{}", kBold, "OWNER",
               "STATE", "TITLE", "DEPS", "DONE", "LIVE", kReset, eol);
  for (const auto& t : tasks) {
    const std::string age =
        t.done_claim.ts >= 0 ? fmtAge(now - t.done_claim.ts) : "—";
    std::println("{:<10} {}{:<10}{} {:<34} {:<10} {:<7} {}{}", t.owner,
                 stateColor(t.state), t.state, kReset, truncate(t.title, 34),
                 truncate(depsCell(t.deps), 10), age, liveCell(t.owner_live),
                 eol);
  }
  std::println("{}", eol);
  std::println("{}{} task(s) — open/in_flight from the `tasks` spine, done "
               "from $STATE/done, liveness from triggers{}{}",
               kDim, tasks.size(), kReset, eol);
}

auto tasksView(std::span<const char* const> args) -> int {
  std::set<std::string> only;
  for (const char* a : args) only.insert(a);
  emitTable(only, /*live=*/false);
  return 0;
}

// `bus tasks watch [OWNER...]` — continuous full-screen refresh, the live
// fleet-activity surface (the pane that replaced bus-deck). Mirrors the
// monitor viewer idiom: alt-screen + EINTR-able signal handling +
// fetch/render/sleep. The one-shot `bus tasks` stays for scripting and the
// dispatch-side B-adoption flow.
auto tasksWatch(std::span<const char* const> args) -> int {
  std::set<std::string> only;
  for (const char* a : args) only.insert(a);
  installInterruptHandlers(onSignalTasks);

  std::print("{}{}", kAltOn, kCursorHide);
  std::fflush(stdout);
  while (gStopTasks == 0) {
    std::print("{}", kCursorHome);
    emitTable(only, /*live=*/true);
    std::print("{}", kClearBelow);
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::print("{}{}", kCursorShow, kAltOff);
  std::fflush(stdout);
  return 0;
}

}  // namespace

auto subTasks(std::span<const char* const> args) -> int {
  if (!args.empty()) {
    const std::string_view verb{args[0]};
    if (verb == "mint-id") return tasksMintId(args.subspan(1));
    if (verb == "open") return tasksOpen(args.subspan(1));
    if (verb == "close") return tasksClose(args.subspan(1));
    if (verb == "watch") return tasksWatch(args.subspan(1));
  }
  return tasksView(args);
}

}  // namespace bus
