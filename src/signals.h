#pragma once

// Signal-handler setup that actually interrupts blocking syscalls.
//
// std::signal on Linux/glibc installs handlers with SA_RESTART, which
// means blocking syscalls (read on an inotify fd, etc.) auto-resume
// after the signal handler returns — the volatile stop flag is never
// re-checked and the process hangs until SIGKILL. Long-running viewers
// (inbox / events / monitor) need EINTR to bubble up, so we install
// via sigaction with sa_flags=0.
//
// SIGHUP is treated as a graceful shutdown signal (same as INT/TERM)
// so a closing pty or detached parent runs the viewer's cleanup
// (kAltOff, etc.) instead of leaving a frozen alt-screen frame.
// SIGPIPE is ignored so a half-closed RPC socket or pty write returns
// EPIPE through errno rather than killing the process silently.

#include <signal.h>

namespace bus {

inline auto installInterruptHandlers(void (*handler)(int)) -> void {
  struct sigaction sa {};
  sa.sa_handler = handler;
  sa.sa_flags = 0;  // NOT SA_RESTART — we want EINTR.
  sigemptyset(&sa.sa_mask);
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGHUP, &sa, nullptr);

  struct sigaction sa_ign {};
  sa_ign.sa_handler = SIG_IGN;
  sa_ign.sa_flags = 0;
  sigemptyset(&sa_ign.sa_mask);
  ::sigaction(SIGPIPE, &sa_ign, nullptr);
}

}  // namespace bus
