#include "harness.h"
#include "json_min.h"
#include "rpc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

// Tests for rpc::call and Server.dispatch robustness.
// rpc.cpp is added to bus_tests in CMakeLists.txt.

using namespace bus;
using namespace bus::rpc;
using namespace std::chrono_literals;

namespace {

// A counter that advances with each call for unique socket paths per test.
auto nextSockPath() -> std::string {
  static int n = 0;
  return "/tmp/claude-bus-test-rpc-" + std::to_string(++n) + ".sock";
}

}  // namespace

// M3: rpc::call must return failure (not hang) when the server accepts but
// never replies. Set a short 200 ms timeout via the env-var override so the
// test finishes quickly.  Bind the margin at 4× the timeout (800 ms) — if
// call() respects the socket timeout it will return in ≤ 200 ms; if it hangs
// we'd exceed the margin and the test would time out the test binary itself.
TEST(rpc_call_times_out_when_server_silent) {
  const std::string sock = nextSockPath();
  ::unlink(sock.c_str());

  // Create a listening socket. This test owns it directly (no Server) so it
  // stays alive but never writes a reply — the accept-and-ignore path.
  const int srv = ::socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(srv >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  ::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
  CHECK(::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
  CHECK(::listen(srv, 4) == 0);

  // Background thread: accept and hold open without replying.
  std::thread acceptor([srv]() {
    const int conn = ::accept(srv, nullptr, nullptr);
    if (conn >= 0) {
      // Park for a while so the test's call() timeout fires first.
      std::this_thread::sleep_for(2s);
      ::close(conn);
    }
  });

  // Use a 200 ms client timeout so the test is fast.
  ::setenv("CLAUDE_BUS_CONN_TIMEOUT_MS", "200", 1);

  const auto t0 = std::chrono::steady_clock::now();
  auto req = json::parse(R"({"op":"ping"})");
  CHECK(req.has_value());
  const auto result = call(sock, *req);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - t0);

  // call() must have returned an error (no reply was sent).
  CHECK(!result.has_value());
  // Must have returned within 4× the timeout, not hung.
  CHECK(elapsed.count() < 800);

  ::unsetenv("CLAUDE_BUS_CONN_TIMEOUT_MS");
  ::close(srv);
  ::unlink(sock.c_str());
  acceptor.join();
}

// N1: a throwing handler must not crash the broker — dispatch must catch the
// exception and return a structured error response.  We run a minimal Server
// in a thread, register a handler that always throws, and verify that
// rpc::call gets an error response rather than a connection reset (which
// would indicate the process crashed or the handler propagated).
//
// NOTE: calls Server::requestStop() at the end, which sets the process-global
// gStopFlag. Keep this test LAST in the file so the flag doesn't affect any
// earlier server test in this binary.
TEST(rpc_dispatch_catches_handler_exception) {
  const std::string sock = nextSockPath();
  Server server{sock};
  CHECK(server.bind());

  // Register a handler that always throws a std::runtime_error.
  server.on("boom", [](const json::Value&) -> json::Value {
    throw std::runtime_error("intentional test exception");
  });

  // Run the server in a background thread (run() blocks until stopped).
  std::thread srv_thread([&server]() {
    server.run();
  });

  // Give the server a moment to enter its accept loop.
  std::this_thread::sleep_for(50ms);

  auto req = json::parse(R"({"op":"boom"})");
  CHECK(req.has_value());
  const auto result = call(sock, *req);

  // The call must succeed at the transport layer (result has a value), and
  // the response must be an error object (ok == false), not a crash.
  CHECK(result.has_value());
  if (result.has_value()) {
    // ok field must be false — the structured error response.
    CHECK(!result->getOrBool("ok", true));
    // error message must be non-empty (contains the exception text).
    const auto err = result->getOrString("error");
    CHECK(!err.empty());
  }

  Server::requestStop();
  srv_thread.join();
  ::unlink(sock.c_str());
}
