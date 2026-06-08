#include "dispatch.h"

#include "pane.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace bus::dispatch {

namespace {

auto sleepFor(std::chrono::milliseconds d) -> void {
  std::this_thread::sleep_for(d);
}

// Sleep up to d, waking every ~50ms to recheck the stop predicate.
// Returns false the moment a shutdown is observed so a multi-second
// backoff can't park the delivery thread past SIGTERM.
auto sleepUnlessStopping(std::chrono::milliseconds d,
                         const std::function<bool()>& should_stop) -> bool {
  constexpr std::chrono::milliseconds kSlice{50};
  for (auto left = d; left > std::chrono::milliseconds::zero();) {
    if (should_stop()) return false;
    const auto chunk = left < kSlice ? left : kSlice;
    std::this_thread::sleep_for(chunk);
    left -= chunk;
  }
  return !should_stop();
}

// Acquire the per-pane TTY flock. The lock is the same one
// `bus msg send NAME ...` uses, so concurrent writers serialize byte-for-
// byte at the TTY (no [bus-wak[bus-wake]e] interleaves).
class FlockGuard {
 public:
  FlockGuard(const std::string& path) {
    fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd_ < 0) return;
    while (::flock(fd_, LOCK_EX) < 0) {
      if (errno != EINTR) {
        ::close(fd_);
        fd_ = -1;
        return;
      }
    }
  }
  ~FlockGuard() {
    if (fd_ >= 0) {
      ::flock(fd_, LOCK_UN);
      ::close(fd_);
    }
  }
  FlockGuard(const FlockGuard&) = delete;
  auto operator=(const FlockGuard&) -> FlockGuard& = delete;
  auto held() const -> bool { return fd_ >= 0; }

 private:
  int fd_{-1};
};

auto isReady(std::string_view agent) -> bool {
  const auto ps = paneStateCached(agent);
  return ps.ok && ps.isInsert() && !ps.hasBuffer();
}

auto normalize(std::string_view pane_id) -> void {
  sendKey(pane_id, "Esc");
  sendKey(pane_id, "Esc");
  sleepFor(std::chrono::milliseconds{100});
  sendKey(pane_id, "i");
  sleepFor(std::chrono::milliseconds{100});
}

}  // namespace

auto dispatchTui(std::string_view state_dir, std::string_view agent,
                 std::string_view body,
                 const std::function<bool()>& should_stop) -> bool {
  const auto pane = paneId(agent);
  if (pane.empty()) return false;

  const std::string lock_dir = std::string{state_dir} + "/tui-locks";
  std::error_code ec;
  fs::create_directories(lock_dir, ec);
  const std::string lock_path =
      lock_dir + "/" + std::string{agent} + ".lock";

  FlockGuard guard{lock_path};
  if (!guard.held()) return false;

  // Three attempts, exponential backoff. Mirrors the existing shell.
  const std::chrono::milliseconds backoffs[] = {
      std::chrono::milliseconds{250},
      std::chrono::milliseconds{1000},
      std::chrono::milliseconds{4000},
  };
  for (int i = 0; i < 3; ++i) {
    if (should_stop()) return false;
    if (isReady(agent)) {
      return sendToPane(pane, body);
    }
    normalize(pane);
    if (!sleepUnlessStopping(backoffs[i], should_stop)) return false;
  }
  return false;
}

}  // namespace bus::dispatch
