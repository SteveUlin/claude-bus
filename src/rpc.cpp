#include "rpc.h"

#include "signals.h"
#include "state_paths.h"

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <print>
#include <string>
#include <thread>
#include <utility>

namespace bus::rpc {

namespace {

std::atomic<int> gStopFlag{0};

auto onStop(int) -> void { gStopFlag.store(1, std::memory_order_release); }

// One parsed request handed from the intake thread to the single
// processing thread, carrying the live connection fd. Processing runs
// the handler, writes the reply to conn_fd, and closes it. Intake never
// touches conn_fd after the handoff (except the backpressure-reject path,
// where the request was never enqueued).
struct QueuedReq {
  json::Value req;
  int conn_fd{-1};
};

// Bounded, mutex-guarded MPSC queue: many intake pushes (one thread today,
// but the bound + locking make it safe regardless), one processing drain.
// This is the ONLY synchronized object in the broker — all broker state
// stays exclusively on the processing thread, so atomicity holds by
// construction (see docs/broker-intake-decouple.md §6).
class CmdQueue {
 public:
  explicit CmdQueue(std::size_t max) : max_{max} {}

  // Push one request. Returns false when full — the caller (intake) then
  // rejects with a constant string, never blocking (a blocked intake would
  // stop draining accept() and re-wedge; §4).
  auto push(QueuedReq qr) -> bool {
    std::lock_guard lk{m_};
    if (q_.size() >= max_) return false;
    q_.push_back(std::move(qr));
    return true;
  }

  // Drain everything pending in one swap (processing thread only).
  auto drain() -> std::deque<QueuedReq> {
    std::lock_guard lk{m_};
    std::deque<QueuedReq> out;
    out.swap(q_);
    return out;
  }

 private:
  std::mutex m_;
  std::deque<QueuedReq> q_;
  std::size_t max_;
};

// Max queued-but-unprocessed RPCs before intake sheds load. Override via
// $CLAUDE_BUS_RPC_QUEUE_MAX (tests). 256 is well above the real fleet's
// concurrent-RPC rate (a handful of agents + ~1 Hz viewer polls); a flood
// past it means processing is wedged, and shedding promptly gives the client
// a fast "busy" instead of parking it behind a deep backlog that may outlast
// its own wait (auri's depth note — keep it shallow rather than 1024-deep).
auto queueMax() -> std::size_t {
  if (const char* e = std::getenv("CLAUDE_BUS_RPC_QUEUE_MAX");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) {
      return static_cast<std::size_t>(v);
    }
  }
  return 256;
}

// Per-direction timeout on an ACCEPTED connection (broker-hardening
// CRIT #2). A legit call() connects, writes, and half-closes within ms;
// 5 s is generous headroom for a slow-but-legit client, while bounding a
// STALLED client that would otherwise park the single intake thread
// forever (RCVTIMEO) or the error-write-back on a full send buffer
// (SNDTIMEO). SO_RCVTIMEO is per-read, so a progressing large body keeps
// getting fresh windows — only a stalled peer is cut.
auto connTimeoutMs() -> std::int64_t {
  if (const char* e = std::getenv("CLAUDE_BUS_CONN_TIMEOUT_MS");
      e != nullptr && *e != '\0') {
    if (const auto v = std::atoll(e); v > 0) return v;
  }
  return 5000;
}

}  // namespace

auto defaultSocketPath() -> std::string {
  return stateRoot() + "/broker.sock";
}

auto Server::requestStop() -> void { gStopFlag.store(1); }

auto Server::stopRequested() -> bool {
  return gStopFlag.load(std::memory_order_acquire) != 0;
}

Server::Server(std::string socket_path) : socket_path_{std::move(socket_path)} {}

Server::~Server() {
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  // Inode-guarded cleanup. The naive `remove(socket_path_)` here used
  // to delete a *successor* broker's socket: if process A was shutting
  // down while process B had already re-bound the same path, A's
  // destructor would unlink B's file out from under it. Check the
  // on-disk inode and only unlink if it's the one we created.
  struct stat st{};
  if (bound_ino_ != 0 && ::stat(socket_path_.c_str(), &st) == 0 &&
      static_cast<std::uint64_t>(st.st_dev) == bound_dev_ &&
      static_cast<std::uint64_t>(st.st_ino) == bound_ino_) {
    ::unlink(socket_path_.c_str());
  }
}

auto Server::on(std::string_view op, Handler h) -> void {
  handlers_.insert_or_assign(std::string{op}, std::move(h));
}

auto Server::bind() -> bool {
  // Ensure parent dir exists.
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path{socket_path_}.parent_path(), ec);

  // Remove stale socket file from a prior crashed broker.
  ::unlink(socket_path_.c_str());

  // SOCK_NONBLOCK so the intake drain loop's accept() returns EAGAIN once
  // the backlog is empty (and breaks back to the stop-responsive pselect)
  // instead of parking in a blocking accept() between requests — a parked
  // accept() swallows SIGTERM (EINTR → re-accept) and wedges shutdown.
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) {
    std::println(stderr, "rpc: socket: {}", std::strerror(errno));
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    std::println(stderr, "rpc: socket path too long: {}", socket_path_);
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  std::strncpy(addr.sun_path, socket_path_.c_str(),
               sizeof(addr.sun_path) - 1);

  if (::bind(listen_fd_,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    std::println(stderr, "rpc: bind {}: {}", socket_path_,
                 std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  // 0600 so only the user can connect — the bus is single-user.
  ::chmod(socket_path_.c_str(), 0600);

  // Record the dev/inode of the bound file. The destructor uses this
  // to avoid clobbering a successor's socket — see ~Server.
  {
    struct stat st{};
    if (::stat(socket_path_.c_str(), &st) == 0) {
      bound_dev_ = static_cast<std::uint64_t>(st.st_dev);
      bound_ino_ = static_cast<std::uint64_t>(st.st_ino);
    }
  }

  if (::listen(listen_fd_, 32) < 0) {
    std::println(stderr, "rpc: listen: {}", std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  return true;
}

namespace {
auto nowMsRt() -> std::int64_t {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

auto Server::dispatch(const json::Value& req) -> json::Value {
  const auto handler_name = req.getOrString("op");
  if (handler_name.empty()) {
    return errorResponse("missing \"op\" field");
  }
  auto it = handlers_.find(handler_name);
  if (it == handlers_.end()) {
    return errorResponse(std::string{"unknown op: "} + handler_name);
  }
  return it->second(req);
}

// Phase 1 (Pillar D / W23 + W24): producer/consumer split. The INTAKE
// thread (this function's outer loop) only accept()s, reads + parses one
// request per connection, and hands the live conn_fd to the PROCESSING
// thread via CmdQueue. Processing — the sole owner of all broker state —
// runs the handler, writes the reply, closes the fd, and drives the
// self-driven tick off its own timerfd. See docs/broker-intake-decouple.md.
//
// Why this shape: handlers and the delivery tick BOTH mutate registry /
// cursors / in-flight / topic logs. Keeping them on one thread (processing)
// preserves today's lock-free atomicity by construction. Intake touches
// only listen_fd, the queue, and constant strings, so it can never starve
// processing AND processing can never starve intake's accept() — the
// fan-out wedge (a slow tick parking the only accept()ing thread) is gone.
auto Server::run(std::chrono::milliseconds tick_interval,
                 std::function<void()> on_tick,
                 std::function<std::int64_t()> next_deadline_ms) -> int {
  if (listen_fd_ < 0) {
    std::println(stderr, "rpc: run() called before bind()");
    return 1;
  }
  installInterruptHandlers(onStop);

  CmdQueue queue{queueMax()};

  // eventfd the intake thread signals after each push so processing wakes
  // promptly to drain + dispatch (rather than waiting out its base-cadence
  // poll). Counting semaphore semantics; we drain the counter and then
  // drain the whole queue, so a coalesced signal still processes every push.
  const int event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (event_fd < 0) {
    std::println(stderr, "rpc: eventfd: {}", std::strerror(errno));
    return 1;
  }

  // ---- processing thread: sole owner of broker state + the tick ----
  auto process_main = [&]() {
    // D8 Part B timerfd, now owned by the processing reactor: armed to the
    // soonest pending deadline so escalation/retry fire deterministically
    // even with zero RPC traffic (the quiet-fleet fix, generalized to the
    // whole tick — W24). Only created when a deadline source is supplied.
    int timer_fd = -1;
    if (on_tick && next_deadline_ms) {
      timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
      if (timer_fd < 0) {
        std::println(stderr,
                     "rpc: timerfd_create: {} (deadlines ride base cadence)",
                     std::strerror(errno));
      }
    }
    // Arm to the next deadline. Disarm only when nothing is pending; clamp a
    // past-due deadline to ~immediate (never 0 — 0 disarms and drops it). The
    // deadline source must stop re-emitting a handled deadline (escalate-once)
    // so a never-clearing condition can't pin the re-arm at 1 ms and busy-loop.
    auto rearm = [&]() {
      if (timer_fd < 0 || !next_deadline_ms) return;
      const std::int64_t d = next_deadline_ms();
      struct itimerspec its {};  // all-zero = disarmed
      if (d > 0) {
        std::int64_t rel = d - nowMsRt();
        if (rel < 1) rel = 1;  // past-due → fire ~immediately, never 0/disarm
        its.it_value.tv_sec = rel / 1000;
        its.it_value.tv_nsec = (rel % 1000) * 1'000'000;
      }
      ::timerfd_settime(timer_fd, 0, &its, nullptr);  // relative arm
    };

    // Drain + answer every queued request in one batch per wake. Runs the
    // handler on THIS thread (the state owner), writes the reply, closes the fd.
    //
    // FD-OWNERSHIP CONTRACT (auri's refinement #1): once a request is
    // ENQUEUED, the processing thread owns close() on EVERY path — normal
    // reply here, or the "shutting down" drain below. The intake side closes
    // only fds it never enqueued (parse/read error, or a backpressure reject).
    // So a conn is closed exactly once, by exactly one side. If the client
    // disconnected while queued, writeAll's ::write yields EPIPE (SIGPIPE is
    // SIG_IGN'd in installInterruptHandlers) → writeAll returns false → we
    // still ::close. No leak (→ EMFILE) and no crash.
    auto serve_queue = [&]() {
      for (auto& qr : queue.drain()) {
        const auto resp = json::serialize(dispatch(qr.req));
        writeAll(qr.conn_fd, resp);
        ::close(qr.conn_fd);
      }
    };

    // Boot tick so the first deadline is computed + the timerfd armed without
    // waiting for an external event (the quiet-fleet self-sustaining chain).
    if (on_tick) {
      on_tick();
      rearm();
    }

    // Base cadence: keep a periodic floor so the self-rate-limited scans
    // (doorbell/token/trim/auto-clear) still get polled on a silent fleet,
    // and so the stop flag is rechecked promptly. 250 ms by default (the
    // historical tick interval); falls back to 250 ms when no tick is wired,
    // purely for stop responsiveness.
    const auto base = tick_interval.count() > 0 ? tick_interval
                                                : std::chrono::milliseconds{250};

    while (!gStopFlag.load(std::memory_order_acquire)) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(event_fd, &rfds);
      int maxfd = event_fd;
      if (timer_fd >= 0) {
        FD_SET(timer_fd, &rfds);
        if (timer_fd > maxfd) maxfd = timer_fd;
      }
      struct timespec ts {};
      ts.tv_sec = base.count() / 1000;
      ts.tv_nsec = (base.count() % 1000) * 1'000'000;
      sigset_t empty;
      sigemptyset(&empty);
      const int r = ::pselect(maxfd + 1, &rfds, nullptr, nullptr, &ts, &empty);
      if (r < 0) {
        if (errno == EINTR) continue;
        std::println(stderr, "rpc: pselect(proc): {}", std::strerror(errno));
        break;
      }
      // Drain the eventfd counter if it fired (coalesced signals are fine —
      // serve_queue drains the WHOLE queue regardless of the count).
      if (event_fd >= 0 && FD_ISSET(event_fd, &rfds)) {
        std::uint64_t v = 0;
        [[maybe_unused]] ssize_t n = ::read(event_fd, &v, sizeof(v));
      }
      if (timer_fd >= 0 && FD_ISSET(timer_fd, &rfds)) {
        std::uint64_t expirations = 0;
        [[maybe_unused]] ssize_t n =
            ::read(timer_fd, &expirations, sizeof(expirations));
      }
      // Answer any pending RPCs FIRST so handlers see current state, then
      // tick so delivery decisions see the just-applied mutations. Drain
      // unconditionally (not only on the eventfd path) to absorb a push that
      // raced in after the last drain.
      serve_queue();
      if (on_tick) {
        on_tick();
        rearm();
      }
    }

    // Clean shutdown: answer any still-queued requests with a constant error
    // so in-flight clients get a reply instead of a connection reset.
    for (auto& qr : queue.drain()) {
      writeAll(qr.conn_fd,
               json::serialize(errorResponse("broker shutting down")));
      ::close(qr.conn_fd);
    }
    if (timer_fd >= 0) ::close(timer_fd);
  };

  std::thread proc_thread{process_main};

  // ---- intake thread (this thread): accept, parse, hand off ----
  // pselect on the listen fd with the base poll so we recheck gStopFlag
  // even when no connection arrives (stop responsiveness). Intake NEVER
  // runs a handler and NEVER blocks on processing — a full queue is shed
  // with a constant string, not a block (a blocked intake would stop
  // draining accept() and reintroduce the wedge).
  const auto intake_poll = std::chrono::milliseconds{250};
  while (!gStopFlag.load(std::memory_order_acquire)) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listen_fd_, &rfds);
    struct timespec ts {};
    ts.tv_sec = intake_poll.count() / 1000;
    ts.tv_nsec = (intake_poll.count() % 1000) * 1'000'000;
    sigset_t empty;
    sigemptyset(&empty);
    const int r =
        ::pselect(listen_fd_ + 1, &rfds, nullptr, nullptr, &ts, &empty);
    if (r < 0) {
      if (errno == EINTR) continue;
      std::println(stderr, "rpc: pselect(intake): {}", std::strerror(errno));
      break;
    }
    if (r == 0) continue;  // poll timeout — recheck stop flag

    // Drain the accept backlog. Each connection carries exactly one request
    // (the client half-closes after writing — see call()), so we read one
    // line, parse it, and hand the fd to processing.
    while (true) {
      const int conn = ::accept(listen_fd_, nullptr, nullptr);
      if (conn < 0) {
        if (errno == EINTR) continue;
        break;  // EAGAIN/EWOULDBLOCK or error → backlog drained
      }
      // CRIT #2: the listen fd is non-blocking, but the accepted conn is
      // not — bound both directions so a silent/stalled client cannot park
      // the single intake thread (read) or the error-write-back (write).
      {
        const auto ms = connTimeoutMs();
        const struct timeval tv{.tv_sec = ms / 1000,
                                .tv_usec = (ms % 1000) * 1000};
        ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
      }
      auto line = readLine(conn);
      if (!line) {
        if (line.error().message != "EOF before newline") {
          writeAll(conn,
                   json::serialize(errorResponse(line.error().message)));
        }
        ::close(conn);
        continue;
      }
      auto req = json::parse(*line);
      if (!req) {
        // Parse error is derived from the input, not from broker state —
        // safe for intake to answer directly.
        writeAll(conn, json::serialize(errorResponse(req.error())));
        ::close(conn);
        continue;
      }
      if (!queue.push(QueuedReq{std::move(*req), conn})) {
        // Backpressure: queue full → processing is wedged or slammed. Shed
        // with a constant string (no broker-state access) and close. CLI
        // RPCs are idempotent one-shots, so a retry is safe.
        writeAll(conn,
                 json::serialize(errorResponse("broker busy, retry")));
        ::close(conn);
        std::println(stderr, "rpc: queue full — shed one request (busy)");
        continue;
      }
      // Wake processing to drain promptly.
      std::uint64_t one = 1;
      [[maybe_unused]] ssize_t n = ::write(event_fd, &one, sizeof(one));
    }
  }

  // Stop requested (signal or `stop` RPC). Kick processing out of its
  // pselect and join it before tearing down fds.
  std::uint64_t one = 1;
  [[maybe_unused]] ssize_t n = ::write(event_fd, &one, sizeof(one));
  proc_thread.join();
  ::close(event_fd);
  return 0;
}

auto readLine(int fd, std::size_t max_bytes)
    -> Result<std::string> {
  std::string out;
  std::array<char, 4096> buf{};
  while (true) {
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) {
        // A stop signal (SIGTERM/INT/HUP) interrupted the read. Bail so the
        // intake loop can observe gStopFlag instead of re-blocking on a
        // half-sent request and swallowing the shutdown.
        if (gStopFlag.load(std::memory_order_acquire)) {
          return std::unexpected{Error{"interrupted by shutdown"}};
        }
        continue;
      }
      // SO_RCVTIMEO expiry (CRIT #2): a stalled client. Bail so the intake
      // thread is never parked waiting on a half-sent request.
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return std::unexpected{Error{"read timeout"}};
      }
      return std::unexpected{Error{std::string{"read: "} + std::strerror(errno)}};
    }
    if (n == 0) {
      if (out.empty()) {
        return std::unexpected{Error{"EOF before newline"}};
      }
      return out;
    }
    for (ssize_t i = 0; i < n; ++i) {
      if (buf[i] == '\n') {
        out.append(buf.data(), static_cast<std::size_t>(i));
        return out;
      }
    }
    out.append(buf.data(), static_cast<std::size_t>(n));
    if (out.size() > max_bytes) {
      return std::unexpected{Error{"line exceeded max_bytes"}};
    }
  }
}

auto writeAll(int fd, std::string_view s) -> bool {
  // Append '\n' so the peer can terminate readLine().
  std::string out{s};
  out += '\n';
  std::size_t off = 0;
  while (off < out.size()) {
    const auto n = ::write(fd, out.data() + off, out.size() - off);
    if (n < 0) {
      if (errno == EINTR) {
        if (gStopFlag.load(std::memory_order_acquire)) return false;
        continue;
      }
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  return true;
}

auto call(const std::string& socket_path, const json::Value& req)
    -> Result<json::Value> {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return std::unexpected{Error{std::string{"socket: "} + std::strerror(errno)}};
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return std::unexpected{Error{"socket path too long"}};
  }
  std::strncpy(addr.sun_path, socket_path.c_str(),
               sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                sizeof(addr)) < 0) {
    const int e = errno;
    ::close(fd);
    return std::unexpected{Error{std::string{"connect "} + socket_path + ": " +
                           std::strerror(e)}};
  }
  const auto wire = json::serialize(req);
  if (!writeAll(fd, wire)) {
    ::close(fd);
    return std::unexpected{Error{"writeAll failed"}};
  }
  // Half-close write side so the server's readLine returns once we're
  // done sending (otherwise it'd wait for \n we already wrote, but
  // shutdown(SHUT_WR) makes the contract explicit either way).
  ::shutdown(fd, SHUT_WR);

  auto line = readLine(fd);
  ::close(fd);
  if (!line) return std::unexpected{line.error()};
  auto parsed = json::parse(*line);
  if (!parsed) return std::unexpected{Error{parsed.error()}};
  return *parsed;
}

auto okResponse() -> json::Value {
  std::map<std::string, json::Value> m;
  m.insert({"ok", json::Value::from(true)});
  return json::Value::fromObject(std::move(m));
}

auto okResponse(std::map<std::string, json::Value> extras) -> json::Value {
  std::map<std::string, json::Value> m = std::move(extras);
  m.insert_or_assign("ok", json::Value::from(true));
  return json::Value::fromObject(std::move(m));
}

auto errorResponse(std::string_view msg) -> json::Value {
  std::map<std::string, json::Value> m;
  m.insert({"ok", json::Value::from(false)});
  m.insert({"error", json::Value::from(std::string{msg})});
  return json::Value::fromObject(std::move(m));
}

}  // namespace bus::rpc
