#include "rpc.h"

#include "signals.h"
#include "state_paths.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <print>
#include <string>
#include <utility>

namespace bus::rpc {

namespace {

std::atomic<int> gStopFlag{0};

auto onStop(int) -> void { gStopFlag.store(1, std::memory_order_release); }

}  // namespace

auto defaultSocketPath() -> std::string {
  return stateRoot() + "/broker.sock";
}

auto Server::requestStop() -> void { gStopFlag.store(1); }

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

  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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

auto Server::run(std::chrono::milliseconds tick_interval,
                 std::function<void()> on_tick) -> int {
  if (listen_fd_ < 0) {
    std::println(stderr, "rpc: run() called before bind()");
    return 1;
  }
  installInterruptHandlers(onStop);

  const bool use_pselect = tick_interval.count() > 0 && on_tick;

  while (!gStopFlag.load(std::memory_order_acquire)) {
    if (use_pselect) {
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(listen_fd_, &rfds);
      struct timespec ts {};
      ts.tv_sec = tick_interval.count() / 1000;
      ts.tv_nsec = (tick_interval.count() % 1000) * 1000000;
      sigset_t empty;
      sigemptyset(&empty);
      const int r =
          ::pselect(listen_fd_ + 1, &rfds, nullptr, nullptr, &ts, &empty);
      if (r < 0) {
        if (errno == EINTR) continue;
        std::println(stderr, "rpc: pselect: {}", std::strerror(errno));
        return 1;
      }
      if (r == 0) {
        on_tick();
        continue;
      }
      // r > 0: connection ready. Drain a backlog before the next tick,
      // but cap the inner-loop duration so on_tick is guaranteed to
      // fire at least every kInnerBudget regardless of RPC volume.
      //
      // History: without this cap, sustained 1-Hz polling from viewer
      // panes (monitor + per-agent agent-bar) combined with slow
      // per-RPC paneState calls (zellij dump-screen sometimes blocks
      // for seconds) kept the inner loop indefinitely topped up. The
      // broker stayed alive, RPCs returned, but the delivery tick
      // never fired — records sat un-acked, in-flight retries never
      // re-armed.
      constexpr auto kInnerBudget = std::chrono::milliseconds{100};
      const auto inner_deadline =
          std::chrono::steady_clock::now() + kInnerBudget;
      while (true) {
        const int conn = ::accept(listen_fd_, nullptr, nullptr);
        if (conn < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        serve(conn);
        ::close(conn);
        if (std::chrono::steady_clock::now() >= inner_deadline) {
          // Yield to the tick. Any queued connections will be picked
          // up by the next outer pselect; the listen socket's kernel
          // backlog keeps them safe.
          break;
        }
        // poll for another pending connection without blocking
        fd_set rfds2;
        FD_ZERO(&rfds2);
        FD_SET(listen_fd_, &rfds2);
        struct timespec zero {};
        if (::pselect(listen_fd_ + 1, &rfds2, nullptr, nullptr, &zero,
                      &empty) <= 0) {
          break;
        }
      }
      // Run the tick after handling RPCs so delivery decisions see the
      // latest state.
      on_tick();
      continue;
    }

    const int conn = ::accept(listen_fd_, nullptr, nullptr);
    if (conn < 0) {
      if (errno == EINTR) continue;
      std::println(stderr, "rpc: accept: {}", std::strerror(errno));
      return 1;
    }
    serve(conn);
    ::close(conn);
  }
  return 0;
}

auto Server::serve(int conn_fd) -> void {
  while (true) {
    auto line = readLine(conn_fd);
    if (!line) {
      // EOF or read error; client disconnected or socket failed.
      if (line.error().message != "EOF before newline") {
        const auto resp = json::serialize(json::errorResponse(line.error().message));
        writeAll(conn_fd, resp);
      }
      return;
    }
    auto req = json::parse(*line);
    if (!req) {
      const auto resp = json::serialize(json::errorResponse(req.error()));
      writeAll(conn_fd, resp);
      continue;
    }
    auto handler_name = req->getOrString("op");
    if (handler_name.empty()) {
      const auto resp =
          json::serialize(json::errorResponse("missing \"op\" field"));
      writeAll(conn_fd, resp);
      continue;
    }
    auto it = handlers_.find(handler_name);
    if (it == handlers_.end()) {
      const auto resp = json::serialize(
          json::errorResponse(std::string{"unknown op: "} + handler_name));
      writeAll(conn_fd, resp);
      continue;
    }
    auto resp = it->second(*req);
    const auto resp_str = json::serialize(resp);
    if (!writeAll(conn_fd, resp_str)) {
      return;
    }
  }
}

auto readLine(int fd, std::size_t max_bytes)
    -> Result<std::string> {
  std::string out;
  std::array<char, 4096> buf{};
  while (true) {
    const auto n = ::read(fd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
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
      if (errno == EINTR) continue;
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

}  // namespace bus::rpc
