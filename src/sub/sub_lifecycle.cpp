// `bus spawn NAME` — lifecycle helper.
//
// Port of bin/spawn-agent from shell. The agent's command-side identity
// (session UUID discovery, color assignment) is still in the shell
// bin/agent-launch; this only ports the outer "create a tab" wrapper.

#include "../sub.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <format>
#include <print>
#include <span>
#include <string>
#include <vector>

namespace bus {

namespace {

// Resolve a sibling binary in the same directory as this executable.
// Lets the spawned tab call `bus agent-bar NAME` without hard-coding
// /home/sulin/claude-bus/bin/.
auto selfDir() -> std::string {
  char buf[4096];
  const auto n = ::readlink("/proc/self/exe", buf, sizeof(buf));
  if (n <= 0) return "/home/sulin/claude-bus/bin";  // fallback
  std::string path(buf, static_cast<std::size_t>(n));
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  return path.substr(0, slash);
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

}  // namespace

auto subSpawn(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus spawn NAME");
    return 2;
  }
  const std::string name{args[0]};
  const std::string bin = selfDir();
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
        pane name="{0}" cwd="/home/sulin/claude-bus" command="{2}" {{
            args "{0}"
        }}
    }}
}}
)LAYOUT",
      name, bus_bin, agent_launch);

  const int rc = runSync({"zellij", "action", "new-tab", "--name",
                          name.c_str(), "--layout-string", layout.c_str()});
  if (rc != 0) {
    std::println(stderr, "bus spawn: zellij action new-tab failed (rc={})", rc);
    return rc < 0 ? 1 : rc;
  }
  return 0;
}

}  // namespace bus
