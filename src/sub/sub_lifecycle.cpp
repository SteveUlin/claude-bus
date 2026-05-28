// `bus spawn NAME` — lifecycle helper.
//
// Port of bin/spawn-agent from shell. The agent's command-side identity
// (session UUID discovery, color assignment) is still in the shell
// bin/agent-launch; this only ports the outer "create a tab" wrapper.

#include "../sub.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <format>
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

}  // namespace

auto subSpawn(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus spawn NAME");
    return 2;
  }
  const std::string name{args[0]};

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
            args "{0}"
        }}
    }}
}}
)LAYOUT",
      name, bus_bin, agent_launch, root);

  const int rc = runSync({"zellij", "action", "new-tab", "--name",
                          name.c_str(), "--layout-string", layout.c_str()});
  if (rc != 0) {
    std::println(stderr, "bus spawn: zellij action new-tab failed (rc={})", rc);
    return rc < 0 ? 1 : rc;
  }
  // Silent success used to leave callers unsure whether anything
  // happened. One-line confirmation on stdout matches the rest of the
  // bus CLI's "did the thing" voice.
  std::println("spawned tab \"{}\"", name);
  return 0;
}

}  // namespace bus
