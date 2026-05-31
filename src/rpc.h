#pragma once

// Line-delimited JSON-RPC over a Unix socket. Each connection
// transports exactly one request + one response, then closes.
// Simple — the broker's call rate is modest and the connect cost is
// the dominant term anyway.

#include "json_min.h"
#include "types.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace bus::rpc {

// Default socket path. Overridable via $CLAUDE_BUS_STATE/broker.sock.
auto defaultSocketPath() -> std::string;

// Handler signature: take the request `json::Value` (already parsed),
// return the response. Handlers should NOT throw; on error return
// `json::errorResponse(...)`.
using Handler = std::function<json::Value(const json::Value& req)>;

class Server {
 public:
  explicit Server(std::string socket_path);
  ~Server();
  Server(const Server&) = delete;
  auto operator=(const Server&) -> Server& = delete;

  // Register a handler for op name. Replaces any prior registration.
  auto on(std::string_view op, Handler h) -> void;

  // Bind the socket. Returns false on failure (path conflict,
  // permission error). The error is logged to stderr.
  auto bind() -> bool;

  // Block accepting + serving connections until requestStop() is
  // called from a signal handler. Returns 0 on clean shutdown.
  //
  // When `tick_interval > 0`, the accept loop uses pselect with that
  // timeout and invokes `on_tick` whenever it fires (no connection
  // arrived in the window). Tick + RPC handlers run on the same
  // thread, so neither racing nor synchronization is needed.
  //
  // `next_deadline_ms` (D8 Part B) makes escalation deterministic instead
  // of riding RPC/viewer-poll ticks: after every tick the loop calls it
  // for the soonest pending deadline (absolute ms-since-epoch, matching
  // bus::nowMs(); 0 = none) and arms a one-shot timerfd to it. The timerfd
  // is just another readable fd in the pselect set — it fires via the r>0
  // path even on a quiet fleet where the r==0 idle-timeout is unreliable.
  // A past-due deadline is clamped to fire ~immediately (run-once-now);
  // callers must stop re-emitting an already-handled deadline (escalate-
  // once) so re-arm can't busy-loop. EINTR/pselect semantics unchanged.
  auto run(std::chrono::milliseconds tick_interval =
               std::chrono::milliseconds{0},
           std::function<void()> on_tick = nullptr,
           std::function<std::int64_t()> next_deadline_ms = nullptr) -> int;

  // Signal the run loop to exit. Safe to call from a signal handler
  // (sets a volatile atomic flag the accept loop polls).
  static auto requestStop() -> void;

 private:
  std::string socket_path_;
  int listen_fd_{-1};
  // dev/inode of the socket file we bound. We record these so the
  // destructor only unlinks the path if it's STILL the file we
  // created — i.e. a successor broker that re-bound the same path
  // doesn't get its socket clobbered by our shutdown.
  std::uint64_t bound_dev_{0};
  std::uint64_t bound_ino_{0};
  std::map<std::string, Handler, std::less<>> handlers_;

  auto serve(int conn_fd) -> void;
};

// One-shot client. Connect, send `req` as a single JSON line, read the
// response line, parse it. The socket is closed after.
auto call(const std::string& socket_path, const json::Value& req)
    -> Result<json::Value>;

// Convenience: read one line ending in '\n' from a connected fd, with
// an overall byte cap. Returns the line WITHOUT the trailing newline.
auto readLine(int fd, std::size_t max_bytes = 1 << 20)
    -> Result<std::string>;

// Convenience: write `s` followed by '\n', repeating writes until all
// bytes are sent or an error occurs.
auto writeAll(int fd, std::string_view s) -> bool;

}  // namespace bus::rpc
