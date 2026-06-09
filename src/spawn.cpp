#include "spawn.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

namespace bus {

namespace {

// Build the execvp argv BEFORE fork. In a multithreaded process the child
// inherits only the forking thread; any other thread holding the malloc arena
// lock at fork time leaves it locked forever in the child, so a heap alloc
// inside the child path (between fork and execvp) can deadlock. Prepare all
// data in the parent. (Mirror of process.cpp's buildArgv.)
auto buildArgv(const std::vector<const char*>& argv) -> std::vector<char*> {
  std::vector<char*> a;
  a.reserve(argv.size() + 1);
  for (const auto* s : argv) a.push_back(const_cast<char*>(s));
  a.push_back(nullptr);
  return a;
}

}  // namespace

auto spawnChild(const std::vector<const char*>& argv,
                std::string_view stdin_bytes) -> pid_t {
  if (argv.empty()) return -1;

  // Prepare argv before fork (arena-lock safety).
  auto a = buildArgv(argv);

  // Pipe: parent writes stdin_bytes → child reads as STDIN_FILENO.
  int p[2]{};
  if (::pipe(p) != 0) return -1;

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(p[0]);
    ::close(p[1]);
    return -1;
  }

  if (pid == 0) {
    // --- child ---
    // Die when the parent (broker) dies, regardless of how it exits.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);

    // Wire stdin from the pipe read end.
    ::dup2(p[0], STDIN_FILENO);
    ::close(p[0]);
    ::close(p[1]);

    // Silence stdout + stderr — trigger scripts are fire-and-forget; their
    // output is not read back by the broker.
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }

    ::execvp(a[0], a.data());
    _exit(127);
  }

  // --- parent ---
  ::close(p[0]);  // child owns the read end

  // Best-effort write: ignore SIGPIPE (child may exit before reading all),
  // and don't retry on short writes — trigger stdin is advisory.
  if (!stdin_bytes.empty()) {
    struct sigaction sa_ign {};
    sa_ign.sa_handler = SIG_IGN;
    struct sigaction sa_old {};
    ::sigaction(SIGPIPE, &sa_ign, &sa_old);

    const auto* buf = stdin_bytes.data();
    auto remaining = static_cast<ssize_t>(stdin_bytes.size());
    while (remaining > 0) {
      const ssize_t n = ::write(p[1], buf, static_cast<std::size_t>(remaining));
      if (n <= 0) break;  // EPIPE or error — best-effort, stop
      buf += n;
      remaining -= n;
    }

    ::sigaction(SIGPIPE, &sa_old, nullptr);
  }

  ::close(p[1]);  // signals EOF to the child's stdin
  return pid;
}

auto reapChildren() -> int {
  int count = 0;
  for (;;) {
    int status = 0;
    const pid_t r = ::waitpid(-1, &status, WNOHANG);
    if (r <= 0) break;  // 0 = nothing ready; -1 = no children
    ++count;
  }
  return count;
}

}  // namespace bus
