// `bus topic inbox NAME` — tail-follow an agent's broker-managed inbox.
//
// Reads /tmp/claude-bus/topics/inbox-NAME.log (the agent-inbox topic)
// via the in-process topic_log layer. inotify-watches for appends and
// pretty-prints each record as a stanza with timestamp + sender.
//
// Was previously /tmp/claude-bus/mailbox/<name>.log via the legacy
// bus::dump API; that path retired in phase 4g.

#include "../broker.h"
#include "../bus.h"
#include "../signals.h"
#include "../sub.h"
#include "../envelope.h"
#include "../journal.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>

namespace fs = std::filesystem;

namespace bus {

namespace {

constexpr std::string_view kReset = "\033[0m";
constexpr std::string_view kBold = "\033[1m";
constexpr std::string_view kDim = "\033[2m";
constexpr std::string_view kCyan = "\033[36m";

volatile std::sig_atomic_t gStopInbox = 0;
auto onSignalInbox(int) -> void { gStopInbox = 1; }

auto formatTs(std::int64_t ms) -> std::string {
  using namespace std::chrono;
  const auto tp = sys_time<seconds>{floor<seconds>(milliseconds{ms})};
  return std::format("{:%FT%TZ}", tp);
}

auto printHeader(std::string_view name, std::size_t loaded) -> void {
  const auto* glyph = loaded > 0 ? "📬" : "📭";
  std::println("{}{} inbox: {}{}   {}({} message{} on load){}", kBold, glyph,
               name, kReset, kDim, loaded, loaded == 1 ? "" : "s", kReset);
}

auto printRecord(const bus::Record& rec) -> void {
  const auto env = bus::msg::decodeEnvelope(rec.payload);
  std::println("");
  std::println("{}🕒 {}{} {}✉️  from {}{}{}", kDim, formatTs(rec.append_ms),
               kReset, kBold, kCyan, env.sender, kReset);
  if (env.body.empty()) return;
  std::string indented = "  ";
  for (char c : env.body) {
    indented += c;
    if (c == '\n') indented += "  ";
  }
  std::println("{}", indented);
}

}  // namespace

auto subInbox(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus topic inbox NAME");
    return 2;
  }
  const std::string name{args[0]};

  installInterruptHandlers(onSignalInbox);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const auto cfg = resolveConfig();
  const auto topic_name = std::string{"inbox-"} + name;
  const auto log_path =
      cfg.state_dir + "/topics/" + topic_name + ".log";

  auto log_r = bus::Journal::open(log_path);
  if (!log_r) {
    std::println(stderr, "inbox: no such topic: {} ({})",
                 topic_name, log_r.error().message);
    return 1;
  }
  auto log = std::move(*log_r);
  auto initial = log.dump();
  if (!initial) {
    std::println(stderr, "inbox: dump failed: {}", initial.error().message);
    return 1;
  }
  printHeader(topic_name, initial->size());
  for (const auto& rec : *initial) printRecord(rec);

  std::unordered_set<std::string> seen;
  for (const auto& rec : *initial) seen.insert(rec.id);

  const auto dir = cfg.state_dir + "/topics";
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    std::println(stderr, "inbox: cannot create {}: {}", dir, ec.message());
    return 1;
  }

  const int infd = ::inotify_init1(IN_CLOEXEC);
  if (infd < 0) {
    std::println(stderr, "inbox: inotify_init1: {}", std::strerror(errno));
    return 1;
  }
  const int wd = ::inotify_add_watch(infd, dir.c_str(),
                                     IN_CREATE | IN_MODIFY | IN_MOVED_TO);
  if (wd < 0) {
    std::println(stderr, "inbox: add_watch {}: {}", dir, std::strerror(errno));
    ::close(infd);
    return 1;
  }

  const std::string watch_name = topic_name + ".log";

  constexpr std::size_t kBufSize = 16 * (sizeof(inotify_event) + NAME_MAX + 1);
  std::array<char, kBufSize> buf{};

  while (!gStopInbox) {
    const auto n = ::read(infd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) {
        if (gStopInbox) break;
        continue;
      }
      std::println(stderr, "inbox: read: {}", std::strerror(errno));
      break;
    }
    bool touched = false;
    std::size_t pos = 0;
    while (pos + sizeof(inotify_event) <= static_cast<std::size_t>(n)) {
      const auto* ev = reinterpret_cast<const inotify_event*>(buf.data() + pos);
      if (ev->len > 0 && std::string_view{ev->name} == watch_name) {
        touched = true;
      }
      pos += sizeof(inotify_event) + ev->len;
    }
    if (!touched) continue;

    auto fresh = log.dump();
    if (!fresh) continue;
    for (const auto& rec : *fresh) {
      if (seen.insert(rec.id).second) printRecord(rec);
    }
  }

  ::inotify_rm_watch(infd, wd);
  ::close(infd);
  return 0;
}

}  // namespace bus
