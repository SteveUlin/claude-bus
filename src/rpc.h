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
// `rpc::errorResponse(...)`.
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

  // Block until requestStop() is called (signal handler or `stop` RPC).
  // Returns 0 on clean shutdown.
  //
  // Producer/consumer split (Pillar D / W23): THIS thread is intake — it
  // accept()s, reads + parses one request per connection, and hands the
  // live fd to a single PROCESSING thread via an internal bounded queue.
  // Processing owns ALL broker state, runs every handler + `on_tick`, and
  // writes each reply. So handlers + the tick stay single-threaded (no
  // locking needed; atomicity by construction) while a slow tick can never
  // starve intake's accept() and vice-versa. A full queue is shed with a
  // "broker busy" error rather than blocking intake. One request per
  // connection (the client half-closes after writing — see call()).
  //
  // When `tick_interval > 0`, processing runs `on_tick` every tick_interval
  // as a base-cadence floor, plus immediately after draining any queued
  // requests (prompt dispatch).
  //
  // `next_deadline_ms` (D8 Part B, generalized to W24): the processing
  // reactor arms a one-shot timerfd to the soonest pending deadline
  // (absolute ms-since-epoch, matching bus::nowMs(); 0 = none) so
  // escalation/retry fire deterministically even with ZERO RPC traffic —
  // the timerfd is a readable fd in processing's pselect set, independent
  // of viewer polling. A past-due deadline is clamped to fire ~immediately;
  // callers must stop re-emitting a handled deadline (escalate-once) so
  // re-arm can't busy-loop.
  auto run(std::chrono::milliseconds tick_interval =
               std::chrono::milliseconds{0},
           std::function<void()> on_tick = nullptr,
           std::function<std::int64_t()> next_deadline_ms = nullptr) -> int;

  // Signal the run loop to exit. Safe to call from a signal handler
  // (sets a volatile atomic flag the accept loop polls).
  // NOTE: requestStop()/stopRequested() operate on a single process-global
  // flag — there is exactly one rpc::Server per process.
  static auto requestStop() -> void;

  // True once requestStop()/SIGTERM has been observed. Lets bounded
  // waits elsewhere (e.g. dispatchTui's backoff) bail promptly on
  // shutdown instead of sleeping out their full budget.
  static auto stopRequested() -> bool;

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

  // Resolve `req`'s "op" to a registered handler and invoke it, or return a
  // structured error. Runs on the processing thread only (it reaches into
  // handler-captured broker state).
  auto dispatch(const json::Value& req) -> json::Value;
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

// Helpers for the common "build a flat RPC result object" pattern.
// These encode the "ok"/"error" protocol field names and belong here,
// not in the domain-agnostic JSON serializer.
auto okResponse() -> json::Value;
auto okResponse(std::map<std::string, json::Value> extras) -> json::Value;
auto errorResponse(std::string_view msg) -> json::Value;

}  // namespace bus::rpc
