#pragma once

// Child-process spawn with output capture and a timeout-kill backstop.
//
// One implementation of "fork + execvp a command, dispose of its stdio, and
// never wait forever." Before this module the exact same machinery lived twice
// — anonymous-namespace copies in pane.cpp (runCapture/runSilent) and
// sub_lifecycle.cpp (runSync/runCaptureLocal). Both drove `zellij action …` /
// `pgrep` subprocesses; neither was testable. This is the single typed seam
// they now share.
//
// The timeout matters: a pane/zellij call on the broker's main pselect thread
// halts the delivery loop if it hangs (scanEvents stops draining acks,
// scanRetries stops firing). The default is far past zellij's normal latency
// (dump-screen returns in <100 ms) while still capping a wedged child.
//
// Error contract: `rc == -1` means spawn failure OR the child blew past the
// timeout (we SIGKILL it and continue); the caller decides what -1 means for
// its semantics. A child that exec-fails exits 127 (execvp convention).

#include <chrono>
#include <string>
#include <vector>

namespace bus::process {

inline constexpr auto kDefaultTimeout = std::chrono::milliseconds{5000};

struct Captured {
  int rc;           // exit code, or -1 on spawn failure / timeout
  std::string out;  // captured stdout (stderr is discarded)
};

// Fork + exec `argv` with stdout captured, stderr → /dev/null. The argv is a
// braced list of NUL-terminated C strings, e.g. {"zellij", "action",
// "dump-screen", "--pane-id", id.c_str()}.
auto runCapture(const std::vector<const char*>& argv,
                std::chrono::milliseconds timeout = kDefaultTimeout)
    -> Captured;

// Fork + exec `argv` with stdin/stdout/stderr → /dev/null. Returns the exit
// code (-1 on spawn failure or timeout).
auto runSilent(const std::vector<const char*>& argv,
               std::chrono::milliseconds timeout = kDefaultTimeout) -> int;

}  // namespace bus::process
