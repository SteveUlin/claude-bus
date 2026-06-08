#include "harness.h"
#include "process.h"

#include <chrono>

// process.h is the extracted spawn module — before it, this fork/exec/capture
// machinery was anonymous-namespace inside pane.cpp and sub_lifecycle.cpp and
// could not be reached from a test. These exercise the contract directly.

using bus::process::runCapture;
using bus::process::runSilent;
using namespace std::chrono_literals;

TEST(process_runSilent_exit_codes) {
  CHECK_EQ(runSilent({"sh", "-c", "exit 0"}), 0);
  CHECK_EQ(runSilent({"sh", "-c", "exit 7"}), 7);
}

TEST(process_runSilent_spawn_failure_is_127) {
  // execvp on a nonexistent binary → the child _exit(127) convention.
  CHECK_EQ(runSilent({"bus-no-such-binary-xyzzy"}), 127);
}

TEST(process_runCapture_stdout_and_code) {
  const auto [rc, out] = runCapture({"sh", "-c", "printf hello"});
  CHECK_EQ(rc, 0);
  CHECK_EQ(out, std::string{"hello"});
}

TEST(process_runCapture_captures_then_reports_nonzero) {
  const auto [rc, out] = runCapture({"sh", "-c", "printf foo; exit 4"});
  CHECK_EQ(rc, 4);
  CHECK_EQ(out, std::string{"foo"});
}

TEST(process_runCapture_discards_stderr) {
  // stderr → /dev/null: only stdout is captured.
  const auto [rc, out] = runCapture({"sh", "-c", "printf out; printf err 1>&2"});
  CHECK_EQ(rc, 0);
  CHECK_EQ(out, std::string{"out"});
}

TEST(process_timeout_kills_and_returns_minus_one) {
  // A child that outlives the budget is SIGKILL'd; rc == -1.
  CHECK_EQ(runSilent({"sh", "-c", "sleep 5"}, 100ms), -1);
}
