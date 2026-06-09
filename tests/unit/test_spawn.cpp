#include "harness.h"
#include "spawn.h"

#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std::chrono_literals;

// Helper: poll fn() until it returns true or the deadline passes.
static auto pollUntil(std::chrono::milliseconds budget, auto fn) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (fn()) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

// ── spawn_returns_pid_without_blocking ─────────────────────────────────────
// spawnChild on a slow child returns a positive pid fast (well under the
// child's sleep duration) — the parent must not wait.
TEST(spawn_returns_pid_without_blocking) {
  const auto t0 = std::chrono::steady_clock::now();
  const pid_t pid = bus::spawnChild({"sh", "-c", "sleep 2"}, "");
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0);

  CHECK(pid > 0);
  CHECK(elapsed < 500ms);  // must return long before the 2-second sleep ends

  // Clean up — we don't want the 2-second child lingering in the test suite.
  if (pid > 0) {
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
  }
}

// ── spawn_delivers_stdin ────────────────────────────────────────────────────
// stdin_bytes are actually delivered to the child's STDIN_FILENO.
TEST(spawn_delivers_stdin) {
  // Unique temp path to avoid cross-test collisions even with parallelism.
  char tmp[] = "/tmp/spawn-test-XXXXXX";
  const int fd = ::mkstemp(tmp);
  if (fd >= 0) ::close(fd);
  ::unlink(tmp);  // let the child create it

  const std::string cmd =
      std::string{"cat > "} + tmp;
  const pid_t pid = bus::spawnChild({"sh", "-c", cmd.c_str()}, "hello-stdin");
  CHECK(pid > 0);

  // Poll for the file to appear and accumulate the expected content.
  const bool appeared = pollUntil(2000ms, [&] {
    FILE* f = std::fopen(tmp, "r");
    if (!f) return false;
    char buf[64]{};
    const auto n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    return n >= 11 && std::string_view{buf, n}.find("hello-stdin") != std::string_view::npos;
  });

  CHECK(appeared);
  ::unlink(tmp);  // clean up

  if (pid > 0) {
    int status = 0;
    ::waitpid(pid, &status, WNOHANG);
  }
}

// ── reap_harvests_finished_child ────────────────────────────────────────────
// After a fast child finishes, reapChildren() returns >=1 on the first call
// and 0 on the second call (no double-reap, no zombie left behind).
TEST(reap_harvests_finished_child) {
  const pid_t pid = bus::spawnChild({"true"}, "");
  CHECK(pid > 0);

  // Poll briefly to let the child either exit or become a zombie. If
  // waitpid(WNOHANG) reaps it here that's fine — reapChildren handles the
  // pid2 child below independently.
  pollUntil(2000ms, [&] {
    int status = 0;
    return ::waitpid(pid, &status, WNOHANG) == pid;
  });

  // If the child was reaped by the poll above it's fine — reapChildren just
  // returns 0 for that one. Restart: spawn again and let ONLY reapChildren reap.
  const pid_t pid2 = bus::spawnChild({"true"}, "");
  CHECK(pid2 > 0);
  // Give it time to exit.
  std::this_thread::sleep_for(200ms);

  const int first = bus::reapChildren();
  CHECK(first >= 1);

  const int second = bus::reapChildren();
  CHECK_EQ(second, 0);
}

// ── dies_with_spawner (PR_SET_PDEATHSIG) ────────────────────────────────────
// A grandchild spawned by a "fake broker" child (FB) gets SIGKILL when FB is
// killed. This proves prctl(PR_SET_PDEATHSIG, SIGKILL) is actually set.
TEST(dies_with_spawner) {
  char gc_pid_file[] = "/tmp/spawn-gc-pid-XXXXXX";
  const int fd = ::mkstemp(gc_pid_file);
  if (fd >= 0) ::close(fd);
  ::unlink(gc_pid_file);

  // Fork the "fake broker" (FB).
  const pid_t fb = ::fork();
  if (fb == 0) {
    // --- FB child ---
    // Spawn the grandchild (a long sleep) and write its pid to the temp file.
    const pid_t gc = bus::spawnChild({"sh", "-c", "sleep 30"}, "");
    if (gc > 0) {
      // Write grandchild pid as text to the file so the test can read it.
      char buf[32]{};
      const int n = ::snprintf(buf, sizeof(buf), "%d\n", static_cast<int>(gc));
      const int wfd = ::open(gc_pid_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (wfd >= 0) {
        const ssize_t written __attribute__((unused)) =
            ::write(wfd, buf, static_cast<std::size_t>(n));
        ::close(wfd);
      }
    }
    // Pause until killed.
    for (;;) ::pause();
    _exit(0);
  }

  CHECK(fb > 0);

  // Read the grandchild pid from the file (poll until FB writes it).
  pid_t gc_pid = -1;
  const bool got_pid = pollUntil(3000ms, [&] {
    FILE* f = std::fopen(gc_pid_file, "r");
    if (!f) return false;
    int v = 0;
    const int r = std::fscanf(f, "%d", &v);
    std::fclose(f);
    if (r == 1 && v > 0) { gc_pid = static_cast<pid_t>(v); return true; }
    return false;
  });
  ::unlink(gc_pid_file);

  CHECK(got_pid);
  CHECK(gc_pid > 0);

  // Verify grandchild is alive before we kill FB.
  CHECK(::kill(gc_pid, 0) == 0);

  // Kill the fake broker — PDEATHSIG should propagate SIGKILL to the grandchild.
  ::kill(fb, SIGKILL);
  int fb_status = 0;
  ::waitpid(fb, &fb_status, 0);  // reap FB

  // Poll until the grandchild is gone (ESRCH = no such process).
  const bool gc_dead = pollUntil(2000ms, [&] {
    return ::kill(gc_pid, 0) == -1 && errno == ESRCH;
  });

  CHECK(gc_dead);
}
