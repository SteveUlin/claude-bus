// `bus spawn NAME` — lifecycle helper.
//
// Port of bin/spawn-agent from shell. The agent's command-side identity
// (session UUID discovery, color assignment) is still in the shell
// bin/agent-launch; this only ports the outer "create a tab" wrapper.

#include "../agent_status.h"
#include "../sub.h"
#include "../broker.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../state_paths.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <print>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus {

namespace {

// Resolve a sibling binary in the same directory as this executable.
// Lets the spawned tab call `bus agent-bar NAME` without hard-coding
// any path; the bus can live anywhere on disk.
auto selfDir() -> std::string {
  char buf[4096];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf));
  if (n <= 0) return ".";  // fallback — almost never hit
  std::string path(buf, static_cast<std::size_t>(n));
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return path.substr(0, slash);
}

// Repo root = parent of selfDir() (since the binary lives in
// $BUS_ROOT/bin/). Allows CLAUDE_BUS_ROOT to override for callers
// running the binary from an out-of-tree build dir.
auto busRoot() -> std::string {
  if (const char* env = std::getenv("CLAUDE_BUS_ROOT");
      env != nullptr && *env != '\0') {
    return env;
  }
  const auto self = selfDir();
  const auto slash = self.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return self.substr(0, slash);
}

auto runSync(const std::vector<const char*>& argv) -> int {
  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    std::vector<char*> a;
    a.reserve(argv.size() + 1);
    for (const auto* s : argv) a.push_back(const_cast<char*>(s));
    a.push_back(nullptr);
    ::execvp(a[0], a.data());
    _exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Capture stdout from a subprocess. Used here to read
// `zellij action query-tab-names` for the pre-spawn dedup check; the
// equivalent helper inside pane.cpp is anonymous-namespace, so we
// repeat the minimum needed (no timeout — zellij query-tab-names
// returns in < 50 ms in practice; if it doesn't the spawn caller
// will notice).
auto runCaptureLocal(const std::vector<const char*>& argv)
    -> std::pair<int, std::string> {
  int pipefd[2]{};
  if (::pipe(pipefd) != 0) return {-1, {}};
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return {-1, {}};
  }
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::close(pipefd[1]);
    std::vector<char*> a;
    a.reserve(argv.size() + 1);
    for (const auto* s : argv) a.push_back(const_cast<char*>(s));
    a.push_back(nullptr);
    ::execvp(a[0], a.data());
    _exit(127);
  }
  ::close(pipefd[1]);
  std::string out;
  std::array<char, 4096> buf{};
  for (;;) {
    const auto n = ::read(pipefd[0], buf.data(), buf.size());
    if (n > 0) {
      out.append(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) break;
    if (errno == EINTR) continue;
    break;
  }
  ::close(pipefd[0]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return {-1, std::move(out)};
  }
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, std::move(out)};
}

// Returns true if the live zellij session already has a tab named
// `name`. On query failure we fall through to "no" — better to let
// zellij itself error on the new-tab call than to refuse spawn
// because we couldn't read the tab list.
auto tabNameExists(std::string_view name) -> bool {
  const auto [rc, out] = runCaptureLocal({"zellij", "action",
                                          "query-tab-names"});
  if (rc != 0) return false;
  std::istringstream in{out};
  std::string line;
  while (std::getline(in, line)) {
    // Trim trailing CR / whitespace; tab names themselves are
    // single-line plain strings (zellij prints them raw).
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' ||
            line.back() == '\t')) {
      line.pop_back();
    }
    if (line == name) return true;
  }
  return false;
}

// True if PID is this agent's live claude — guards against killing a stale or
// recycled PID from the pidfile. Reads /proc/<pid>/cmdline (NUL-separated argv);
// the live launch is `claude --name <name>` so both tokens appear.
auto isClaudeFor(pid_t pid, const std::string& name) -> bool {
  std::ifstream f{"/proc/" + std::to_string(pid) + "/cmdline",
                  std::ios::binary};
  if (!f) return false;
  std::string c{std::istreambuf_iterator<char>(f),
                std::istreambuf_iterator<char>()};
  for (auto& ch : c) {
    if (ch == '\0') ch = ' ';
  }
  return c.find("claude") != std::string::npos &&
         c.find(name) != std::string::npos;
}

// Fallback PID resolution when the pidfile is missing/stale or points at the
// wrong process (e.g. the caged systemd-run path, where $$ is the launcher not
// claude). `pgrep -f "claude --name <name>"` matches the live process's full
// command line. Returns the first match, or -1.
auto pgrepClaude(const std::string& name) -> pid_t {
  const std::string pat = "claude --name " + name;
  const auto [rc, out] = runCaptureLocal({"pgrep", "-f", pat.c_str()});
  if (rc != 0) return -1;
  std::istringstream in{out};
  long p = -1;
  in >> p;
  return p > 1 ? static_cast<pid_t>(p) : -1;
}

}  // namespace

auto subSpawn(std::span<const char* const> args) -> int {
  // bus spawn [--role ROLE] [--project-dir DIR] [--profile PROFILE] NAME
  //   --role        passed through to agent-launch (role-injected peer).
  //   --project-dir target repo for the agent's workspace + cwd (e.g.
  //                 ~/taro); the agent stays a claude-bus citizen. Omitted =
  //                 claude-bus, unchanged.
  //   --profile     worker|research|orchestrator, passed through to
  //                 agent-launch (permission/bypass/cage envelope). Omitted =
  //                 agent-launch's default (worker). agent-launch validates it.
  std::string name, role, project_dir, profile;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string a{args[i]};
    if (a == "--role" && i + 1 < args.size()) {
      role = args[++i];
    } else if (a == "--project-dir" && i + 1 < args.size()) {
      project_dir = args[++i];
    } else if (a == "--profile" && i + 1 < args.size()) {
      profile = args[++i];
    } else if (a.starts_with("--")) {
      std::println(stderr,
                   "usage: bus spawn [--role ROLE] [--project-dir DIR] [--profile PROFILE] NAME");
      return 2;
    } else if (name.empty()) {
      name = a;
    } else {
      std::println(stderr,
                   "usage: bus spawn [--role ROLE] [--project-dir DIR] [--profile PROFILE] NAME");
      return 2;
    }
  }
  if (name.empty()) {
    std::println(stderr,
                 "usage: bus spawn [--role ROLE] [--project-dir DIR] [--profile PROFILE] NAME");
    return 2;
  }

  // Dedup: refuse if a tab with this name already exists. Calling
  // `zellij action new-tab` is non-idempotent — repeated invocations
  // happily create N parallel tabs with the same display name, which
  // collides with the bus's "one pane per agent" assumption. The
  // check is best-effort: a query failure falls through to the
  // create call rather than blocking.
  if (tabNameExists(name)) {
    std::println(stderr,
                 "bus spawn: tab \"{}\" already exists. Switch to it "
                 "with `zellij action go-to-tab-name {}` instead.",
                 name, name);
    return 1;
  }

  const std::string bin = selfDir();
  const std::string root = busRoot();
  // Resolve at call time so the layout-string uses the right binary.
  // Note: the old name "bus-new" lived here during phase 4a; the rename
  // landed in 4a.6 but the layout string kept pointing at the dead
  // path, silently producing tabs without an agent-bar.
  const std::string bus_bin = bin + "/bus";
  const std::string agent_launch = bin + "/agent-launch";

  // agent-launch's arg line (KDL): forward --role / --project-dir / --profile
  // when set, then NAME. agent-launch resolves BUS_ROOT from its own path, so
  // the bus side stays claude-bus regardless of --project-dir.
  std::string launch_args = "args";
  if (!role.empty()) launch_args += " \"--role\" \"" + role + "\"";
  if (!project_dir.empty())
    launch_args += " \"--project-dir\" \"" + project_dir + "\"";
  if (!profile.empty()) launch_args += " \"--profile\" \"" + profile + "\"";
  launch_args += " \"" + name + "\"";

  // Pane cwd = the project repo when targeting one, else claude-bus. (agent-
  // launch cd's into the per-agent workspace itself; this is just the start.)
  const std::string pane_cwd = project_dir.empty() ? root : project_dir;

  // The layout-string mirrors layouts/fleet.kdl's agent_tab template:
  // tab-bar plugin, agent-bar strip, claude pane, status-bar plugin.
  const std::string layout = std::format(
      R"LAYOUT(
layout {{
    default_tab_template {{
        pane size=1 borderless=true {{
            plugin location="tab-bar"
        }}
        children
        pane size=1 borderless=true {{
            plugin location="status-bar"
        }}
    }}
    tab name="{0}" {{
        pane size=1 borderless=true name="{0}-bar" command="{1}" {{
            args "agent-bar" "{0}"
        }}
        pane name="{0}" cwd="{3}" command="{2}" {{
            {4}
        }}
    }}
}}
)LAYOUT",
      name, bus_bin, agent_launch, pane_cwd, launch_args);

  const int rc = runSync({"zellij", "action", "new-tab", "--name",
                          name.c_str(), "--layout-string", layout.c_str()});
  if (rc != 0) {
    std::println(stderr, "bus spawn: zellij action new-tab failed (rc={})", rc);
    return rc < 0 ? 1 : rc;
  }
  // P4 — persist this dynamic peer so a fleet-layout relaunch restores it.
  // fleet.kdl agents never come through `bus spawn`, so the registry holds
  // exactly the dynamically-spawned peers (kilvin, the taro spokes, ...).
  // One file per peer under $STATE/dynamic-peers/ — atomic add/remove, no
  // rewrite races. Records the respawn spec (role + project_dir + profile);
  // session continuity is free (agent-launch --continue resumes by
  // name+workspace). profile MUST persist or a restore-peers relaunch silently
  // demotes an orchestrator back to the default (worker) envelope.
  {
    namespace fs = std::filesystem;
    const std::string reg_dir = stateRoot() + "/dynamic-peers";
    std::error_code ec;
    fs::create_directories(reg_dir, ec);
    std::ofstream out{reg_dir + "/" + name};
    if (out) {
      out << "role=" << role << "\n"
          << "project_dir=" << project_dir << "\n"
          << "profile=" << profile << "\n";
    }
  }

  // Silent success used to leave callers unsure whether anything
  // happened. One-line confirmation on stdout matches the rest of the
  // bus CLI's "did the thing" voice.
  std::println("spawned tab \"{}\"", name);
  return 0;
}

// P4 — re-spawn every persisted dynamic peer. Run automatically at fleet
// startup (a step in fleet.kdl) so a layout relaunch restores kilvin + the
// taro spokes with no manual re-spawn. Safe to re-run: subSpawn dedups on
// tab-name-exists, so peers already up are skipped. Each re-spawn RESUMES
// the peer's prior session (agent-launch --continue keys on name+workspace),
// bringing back its in-progress context, not a fresh agent.
auto subRestorePeers(std::span<const char* const>) -> int {
  namespace fs = std::filesystem;
  const std::string reg_dir = stateRoot() + "/dynamic-peers";
  std::error_code ec;
  if (!fs::exists(reg_dir, ec)) {
    std::println("restore-peers: no dynamic peers registered");
    return 0;
  }
  int registered = 0, spawned = 0;
  for (const auto& entry : fs::directory_iterator(reg_dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    std::string role, project_dir, profile, line;
    std::ifstream in{entry.path()};
    while (std::getline(in, line)) {
      if (line.starts_with("role=")) role = line.substr(5);
      else if (line.starts_with("project_dir=")) project_dir = line.substr(12);
      else if (line.starts_with("profile=")) profile = line.substr(8);
    }
    ++registered;
    std::vector<std::string> a;
    if (!role.empty()) { a.emplace_back("--role"); a.push_back(role); }
    if (!project_dir.empty()) {
      a.emplace_back("--project-dir");
      a.push_back(project_dir);
    }
    if (!profile.empty()) { a.emplace_back("--profile"); a.push_back(profile); }
    a.push_back(name);
    std::vector<const char*> argv;
    argv.reserve(a.size());
    for (const auto& s : a) argv.push_back(s.c_str());
    // subSpawn returns 0 on a fresh spawn, non-zero when the tab already
    // exists (dedup) — both are fine; count the fresh ones.
    if (subSpawn(argv) == 0) ++spawned;
  }
  std::println("restore-peers: {} registered, {} newly spawned ({} already up)",
               registered, spawned, registered - spawned);
  return 0;
}

// P4 — explicit teardown verb. Prunes the registry entry (so the peer is NOT
// restored on the next relaunch) and, if its tab is still up, closes it.
// "done" is not machine-detectable, so persistence is the default and removal
// is explicit. Also the building block for agent-culling (P6).
auto subDespawn(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus despawn NAME");
    return 2;
  }
  const std::string name{args[0]};
  namespace fs = std::filesystem;
  std::error_code ec;
  const bool pruned = fs::remove(stateRoot() + "/dynamic-peers/" + name, ec);
  // Close the tab only if it exists AND we can focus it first — gating the
  // close on go-to-tab-name succeeding avoids ever closing the wrong (merely
  // focused) tab if this peer's tab already vanished.
  if (tabNameExists(name) &&
      runSync({"zellij", "action", "go-to-tab-name", name.c_str()}) == 0) {
    runSync({"zellij", "action", "close-tab"});
  }

  // Reap the peer's broker topics (inbox-NAME / commands-NAME) so a
  // despawn leaves no broker residue. Best-effort: a down broker or a
  // peer with unread mail (the gc drained-gate skips that) must not fail
  // the despawn — the prune + tab-close already happened. Targeted mode
  // (agent=NAME) is explicit operator intent, so it skips the live-agent
  // gate but still honors the drained gate.
  int reaped = 0;
  {
    const auto cfg = resolveConfig();
    std::map<std::string, json::Value> m;
    m.insert({"op", json::Value::from("gc")});
    m.insert({"agent", json::Value::from(name)});
    if (auto resp = rpc::call(cfg.socket_path,
                              json::Value::fromObject(std::move(m)));
        resp && resp->getOrBool("ok")) {
      if (const auto* r = resp->get("reaped"); r && r->isArray()) {
        reaped = static_cast<int>(r->asArray().size());
      }
    }
  }
  std::println("despawned {}{} ({} broker topic(s) reaped)", name,
               pruned ? "" : " (was not in the dynamic-peer registry)",
               reaped);
  return 0;
}

// `bus recover <agent> [--no-verify] [--timeout-ms N]` — the relaunch primitive
// (docs/bus-recover.md; P2 Phase C dep). KILL the wedged claude; the pane's
// fleet.kdl respawn-loop relaunches it (fresh `claude --continue`). VERIFY by
// waiting for a fresh events.jsonl line for the agent. REPORT exit code + one
// JSON line. STATELESS w.r.t. recovery policy — the broker's engine decides
// WHETHER to recover (guard) and records the outcome (ledger); this verb is the
// dumb, idempotent, safe-to-call primitive. Exit codes (the breaker's signal):
//   0  recovered + verified (fresh claude up, fresh event landed)
//   10 relaunched but verify timed out (process up, no event yet — boot-stuck)
//   20 failed (no relaunch came up)
//   30 attached-pane defer (human on the pane; did nothing)
//   2  usage / refused target
auto subRecover(std::span<const char* const> args) -> int {
  std::string name;
  bool no_verify = false;
  long timeout_ms = 12000;  // §6: 10-15s boot headroom
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string a{args[i]};
    if (a == "--no-verify") {
      no_verify = true;
    } else if (a == "--timeout-ms" && i + 1 < args.size()) {
      timeout_ms = std::strtol(args[++i], nullptr, 10);
    } else if (a.starts_with("--") || !name.empty()) {
      std::println(stderr, "usage: bus recover <agent> [--no-verify] "
                           "[--timeout-ms N]");
      return 2;
    } else {
      name = a;
    }
  }
  if (name.empty()) {
    std::println(stderr,
                 "usage: bus recover <agent> [--no-verify] [--timeout-ms N]");
    return 2;
  }
  // Never the broker/ops pane — recover only targets agent panes (§7).
  if (name == "broker") {
    std::println(stderr, "bus recover: refusing to target the broker pane");
    return 2;
  }

  namespace fs = std::filesystem;
  const std::string state = stateRoot();
  std::error_code ec;
  fs::create_directories(state + "/recovery", ec);

  const auto t0 = std::chrono::steady_clock::now();
  const auto elapsed = [&] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0)
        .count();
  };
  // One JSON line to stdout — the audit record the engine's ledger folds in.
  const auto emit = [&](long killed_pid, bool relaunched, bool verified,
                        const char* error) {
    const std::string err =
        error ? std::format("\"{}\"", error) : std::string{"null"};
    std::println(
        R"({{"agent":"{}","action":"recover","killed_pid":{},"relaunched":{},)"
        R"("verified":{},"elapsed_ms":{},"session_resumed":{},"error":{}}})",
        name, killed_pid, relaunched ? "true" : "false",
        verified ? "true" : "false", elapsed(), verified ? "true" : "false",
        err);
  };

  // 1. Per-agent flock — two concurrent recovers can't double-fire (§7). The
  //    lock releases when this fd closes at return.
  const int lockfd = ::open((state + "/recovery/" + name + ".lock").c_str(),
                            O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (lockfd < 0 || ::flock(lockfd, LOCK_EX) != 0) {
    if (lockfd >= 0) ::close(lockfd);
    emit(0, false, false, "lock-failed");
    return 20;
  }

  // 2. Attached-pane defer — the human holds the pane; never kill under them.
  if (hasPresenceFile(name)) {
    ::close(lockfd);
    emit(0, false, false, "attached");
    return 30;
  }

  // 3. Resolve the wedged PID: pidfile (deterministic), pgrep fallback.
  pid_t pid = -1;
  {
    std::ifstream pf{state + "/pids/" + name};
    long p = -1;
    if (pf && (pf >> p) && p > 1) pid = static_cast<pid_t>(p);
  }
  if (pid > 1 && !isClaudeFor(pid, name)) pid = -1;  // stale/recycled
  if (pid <= 1) pid = pgrepClaude(name);

  // 4. The events.jsonl byte offset is the "fresh event" boundary for verify —
  //    any line appended past it (the respawn's SessionStart/first hook) proves
  //    a fresh process booted. Avoids parsing ISO-8601 timestamps.
  const std::string events = state + "/events.jsonl";
  std::uintmax_t off = fs::file_size(events, ec);
  if (ec) off = 0;

  // 5. Kill (SIGTERM, 3s grace, SIGKILL backstop). Idempotent: if nothing live
  //    is found, skip straight to verify — the respawn-loop brings it up anyway.
  long killed_pid = 0;
  if (pid > 1 && ::kill(pid, 0) == 0) {
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 30 && ::kill(pid, 0) == 0; ++i) ::usleep(100 * 1000);
    if (::kill(pid, 0) == 0) ::kill(pid, SIGKILL);
    killed_pid = pid;
  }

  // 6a. --no-verify: return as soon as a fresh process is up (caller re-checks).
  if (no_verify) {
    bool up = false;
    while (elapsed() < timeout_ms) {
      if (pgrepClaude(name) > 1) {
        up = true;
        break;
      }
      ::usleep(200 * 1000);
    }
    ::close(lockfd);
    emit(killed_pid, up, false, up ? nullptr : "no-relaunch");
    return up ? 0 : 20;
  }

  // 6b. Event-based verify: poll events.jsonl[off:] for the RESPAWN's first
  //    event — a fresh SessionStart for <name>. CRUCIAL: match the event TYPE,
  //    not just the agent name. The kill in step 5 makes the dying claude emit a
  //    SessionEnd that ALSO carries "agent":"<name>" and lands past `off`; a bare
  //    agent-name match counts that death as a relaunch (false-positive
  //    verified:true → the auto-recovery engine believes a dead agent recovered).
  //    Only a fresh SessionStart (the respawn's `claude --continue`, source
  //    "resume"/"startup") proves it came back. An in-session SessionStart with
  //    source "compact" is a compaction, NOT a relaunch — exclude it.
  const std::string agentNeedle = "\"agent\":\"" + name + "\"";
  const std::string startNeedle = "\"event\":\"SessionStart\"";
  const std::string compactNeedle = "\"source\":\"compact\"";
  bool verified = false;
  while (elapsed() < timeout_ms && !verified) {
    std::ifstream in{events, std::ios::binary};
    if (in) {
      in.seekg(static_cast<std::streamoff>(off));
      std::string line;
      while (std::getline(in, line)) {
        if (line.find(agentNeedle) != std::string::npos &&
            line.find(startNeedle) != std::string::npos &&
            line.find(compactNeedle) == std::string::npos) {
          verified = true;
          break;
        }
      }
    }
    if (!verified) ::usleep(250 * 1000);
  }

  // 7. At verify-timeout, a live process with no event = boot-stuck (10); no
  //    process at all = relaunch never came up (20). Verified = 0.
  const bool relaunched = verified || pgrepClaude(name) > 1;
  ::close(lockfd);
  if (verified) {
    emit(killed_pid, true, true, nullptr);
    return 0;
  }
  if (relaunched) {
    emit(killed_pid, true, false, "verify-timeout");
    return 10;
  }
  emit(killed_pid, false, false, "no-relaunch");
  return 20;
}

}  // namespace bus
