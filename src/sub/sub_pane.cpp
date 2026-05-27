// Subcommands `bus pane-id`, `bus pane-state`, `bus send`.
//
// Thin CLI wrappers over the in-process helpers in src/pane.{h,cpp}.
// Output matches the old bin/pane-id / bin/pane-state / bin/send shell
// scripts byte-for-byte so existing callers keep working during phase
// 4a's coexistence window.

#include "../bus.h"
#include "../pane.h"
#include "../sub.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace bus {

auto subPaneId(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus pane-id NAME");
    return 2;
  }
  const std::string name{args[0]};
  const auto pid = paneId(name);
  if (pid.empty()) {
    // We don't disambiguate miss-vs-ambiguous here for parity with the
    // existing shell that returns 1 in both cases; the ambiguous case
    // is rare in practice (unique titles per agent).
    std::println(stderr, "pane-id: no pane titled \"{}\"", name);
    return 1;
  }
  std::println("{}", pid);
  return 0;
}

auto subPaneState(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus pane-state NAME");
    return 2;
  }
  const std::string name{args[0]};
  const auto ps = paneState(name);
  if (!ps.ok) {
    // Mirror the old bin/pane-state behavior: print to stderr and
    // exit 1 when the pane lookup or dump-screen failed.
    const auto pid = paneId(name);
    if (pid.empty()) {
      std::println(stderr, "pane-state: no pane titled \"{}\"", name);
    } else {
      std::println(stderr, "pane-state: dump-screen failed for {}", pid);
    }
    return 1;
  }

  // Output format matches the old binary exactly so awk-based parsers
  // in agent_status.cpp, drain-mailbox.sh, and dispatch-tui keep
  // working unchanged.
  std::println("pane:        {}", paneId(name));
  std::println("mode:        {}", ps.mode);
  std::println("buffer:      {}", ps.buffer);
  std::println("suggestion:  {}", ps.suggestion);
  std::println("bypass_perms: {}", ps.bypass_perms);
  return 0;
}

// `bus send NAME TEXT` — resolve name to pane, hold the per-pane TTY
// flock, write. Replaces the old shell `bin/bus send`: same single-
// writer guarantee for TTY-writes the broker / dispatch-tui rely on.
auto subSend(std::span<const char* const> args) -> int {
  if (args.size() != 2) {
    std::println(stderr, "usage: bus send NAME TEXT");
    return 2;
  }
  const std::string name{args[0]};
  const std::string text{args[1]};

  const auto pane = paneId(name);
  if (pane.empty()) {
    std::println(stderr, "bus send: no pane titled \"{}\"", name);
    return 1;
  }

  // Single-writer flock for the pane's TTY. Same lockfile path the
  // existing bin/dispatch-tui shell + the watcher's nudges agree on,
  // so concurrent writers (broker delivery, dispatch retries,
  // [bus-wake] nudges) serialize byte-for-byte at the TTY.
  const char* state_env = std::getenv("CLAUDE_BUS_STATE");
  const std::string state =
      state_env ? state_env : std::string{"/tmp/claude-bus"};
  const std::string lock_dir = state + "/tui-locks";
  std::error_code ec;
  std::filesystem::create_directories(lock_dir, ec);
  const std::string lockfile = lock_dir + "/" + name + ".lock";

  const int fd = ::open(lockfile.c_str(),
                        O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  if (fd < 0) {
    std::println(stderr, "bus send: open lockfile {}: {}", lockfile,
                 std::strerror(errno));
    return 1;
  }
  while (::flock(fd, LOCK_EX) < 0) {
    if (errno != EINTR) {
      std::println(stderr, "bus send: flock {}: {}", lockfile,
                   std::strerror(errno));
      ::close(fd);
      return 1;
    }
  }
  // Use the safe path so we preserve the user's draft, normalize VISUAL/
  // NORMAL → INSERT, and defer when the pane is scrolled away.
  const bool ok = sendToPaneSafe(name, text);
  ::flock(fd, LOCK_UN);
  ::close(fd);

  if (!ok) {
    std::println(stderr, "bus send: write to {} ({}) failed or pane "
                         "not ready (scrolled / LOCKED)", name, pane);
    return 1;
  }
  return 0;
}

// `bus pane-send PANE_ID TEXT` — raw write to a pane id, no flock.
// Used by helpers (dispatch-tui shell, broker delivery) that already
// hold the per-pane lock themselves and just want the low-level
// zellij write-chars + send-keys Enter pair.
auto subPaneSend(std::span<const char* const> args) -> int {
  if (args.size() != 2) {
    std::println(stderr, "usage: bus pane-send PANE_ID TEXT");
    return 2;
  }
  const std::string pane{args[0]};
  const std::string text{args[1]};
  if (!sendToPane(pane, text)) {
    std::println(stderr, "bus pane-send: write to {} failed", pane);
    return 1;
  }
  return 0;
}

}  // namespace bus
