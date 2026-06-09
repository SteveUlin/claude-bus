#include "process.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <thread>
#include <utility>

namespace bus::process {

namespace {

// Wait for `pid` up to `timeout`, polling waitpid(WNOHANG) every ~20 ms.
// Returns {timed_out, status}. On timeout the child is SIGKILL'd and reaped
// synchronously — a hung `zellij action …` is wedged on the zellij server IPC
// and won't notice SIGTERM in a useful timeframe.
auto waitWithTimeoutOrKill(pid_t pid, std::chrono::milliseconds timeout)
    -> std::pair<bool /*timed_out*/, int /*status*/> {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) return {false, status};
    if (r < 0) {
      if (errno == EINTR) continue;
      return {false, 0};  // already reaped or other error
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  ::kill(pid, SIGKILL);
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return {true, status};
}

// Drain a pipe FD into a string with a deadline. The fd must be O_NONBLOCK.
// Stops on EOF, deadline-exceeded, or read error.
auto slurpFdWithDeadline(int fd, std::chrono::steady_clock::time_point deadline)
    -> std::string {
  std::string out;
  std::array<char, 4096> buf{};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n > 0) {
      out.append(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n == 0) break;  // EOF
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
      continue;
    }
    break;  // genuine read error
  }
  return out;
}

// Build the execvp argv BEFORE fork. In a multithreaded process the child
// inherits only the forking thread; any other thread holding the malloc arena
// lock at fork time leaves it locked forever in the child, so a heap alloc
// (vector push_back) between fork and execvp can deadlock. Keep the child path
// async-signal-safe (syscalls only).
auto buildArgv(const std::vector<const char*>& argv) -> std::vector<char*> {
  std::vector<char*> a;
  a.reserve(argv.size() + 1);
  for (const auto* s : argv) a.push_back(const_cast<char*>(s));
  a.push_back(nullptr);
  return a;
}

}  // namespace

auto runCapture(const std::vector<const char*>& argv,
                std::chrono::milliseconds timeout) -> Captured {
  int pipefd[2]{};
  // O_CLOEXEC: the pipe fds must not leak into zellij children the broker
  // forks. The child side dup2s pipefd[1] to STDOUT_FILENO before exec —
  // dup2 always clears CLOEXEC on the destination fd, so stdout survives
  // exec correctly while the raw pipefd[1] is closed before exec.
  if (::pipe2(pipefd, O_CLOEXEC) != 0) return {-1, {}};

  auto a = buildArgv(argv);

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
    // stderr to /dev/null — failures surface through exit codes, not noisy
    // stderr leakage.
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::execvp(a[0], a.data());
    _exit(127);
  }

  ::close(pipefd[1]);
  // Non-blocking reads so the deadline drain doesn't get stuck on a hung child
  // that hasn't closed the pipe yet.
  const int flags = ::fcntl(pipefd[0], F_GETFL, 0);
  if (flags >= 0) ::fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto out = slurpFdWithDeadline(pipefd[0], deadline);
  ::close(pipefd[0]);

  // Remaining time after the drain. If we already hit the deadline drain-side,
  // waitWithTimeoutOrKill gets a zero budget and kills immediately.
  const auto wait_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  const auto [timed_out, status] = waitWithTimeoutOrKill(
      pid, wait_budget.count() > 0 ? wait_budget
                                   : std::chrono::milliseconds{0});
  if (timed_out) return {-1, std::move(out)};
  const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {code, std::move(out)};
}

auto runSilent(const std::vector<const char*>& argv,
               std::chrono::milliseconds timeout) -> int {
  auto a = buildArgv(argv);

  const pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    ::execvp(a[0], a.data());
    _exit(127);
  }
  const auto [timed_out, status] = waitWithTimeoutOrKill(pid, timeout);
  if (timed_out) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace bus::process
