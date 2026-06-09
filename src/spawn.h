#pragma once

// Fire-and-forget child spawn for broker triggers (docs/broker-triggers.md §5).
//
// Different contract from process.h, which blocks and captures output. This
// primitive never waits: the parent records the pid and returns immediately.
// The child is tied to the spawner's lifetime via PR_SET_PDEATHSIG(SIGKILL) —
// the kernel delivers SIGKILL to the child if the spawner exits, regardless of
// how it exits. Explicitly NOT setsid/double-fork/detached — a detached child
// would outlive the broker, which is the one hard invariant (§5).
//
// Call reapChildren() once per broker tick to harvest finished trigger children
// without blocking. The WNOHANG sweep prevents zombie accumulation without
// installing a SIGCHLD handler in the hot pselect loop.

#include <sys/types.h>

#include <string_view>
#include <vector>

namespace bus {

// Fork + exec argv[0] with:
//   - stdin fed from stdin_bytes (pipe; write end closed by parent after write)
//   - stdout and stderr → /dev/null
//   - PR_SET_PDEATHSIG(SIGKILL) set before exec
//
// argv must be null-terminated by convention (buildArgv appends nullptr).
// Returns the child pid (>0) on success, -1 on fork failure.
// Does NOT wait; caller must call reapChildren() periodically.
auto spawnChild(const std::vector<const char*>& argv,
                std::string_view stdin_bytes) -> pid_t;

// Non-blocking sweep: waitpid(-1, WNOHANG) until no more finished children.
// Returns the count of children reaped. Call once per broker tick.
//
// CAVEAT (re-check before slice-2 wiring): this reaps ANY child (waitpid(-1)),
// so it must not run concurrently with a targeted waitpid(pid) elsewhere — e.g.
// process.cpp's blocking runCapture/runSilent — or the sweep could harvest that
// call's child first and make its waitpid fail ECHILD (a false spawn-failure).
// Safe today: the broker loop is single-threaded and process.cpp reaps
// synchronously within one call, so they never overlap a tick-level reap.
auto reapChildren() -> int;

}  // namespace bus
