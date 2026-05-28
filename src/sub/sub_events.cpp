// `bus events [--since TS] [--agent NAME]`
//
// Pretty-printed tail of /tmp/claude-bus/events.jsonl. Replaces the
// `tail -f | jq` shell wrapper at bin/tail-events. The output format
// is human-readable column-aligned rather than JSON; the raw log is
// right there for anyone wanting machine-parseable bytes.

#include "../bus.h"
#include "../broker.h"
#include "../topic_log.h"
#include "../signals.h"
#include "../sub.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace bus {

namespace {

volatile std::sig_atomic_t gStopEvents = 0;
auto onSignalEvents(int) -> void { gStopEvents = 1; }

// Pull `"key":"value"` from a JSONL line. Naive — no escape handling.
// log-event.sh writes plain ASCII for the fields we look up (ts,
// agent, event, tool_name, prompt). A real parser would handle
// escapes; we don't need it here.
auto extractStr(std::string_view line, std::string_view key) -> std::string {
  std::string pat;
  pat.reserve(key.size() + 4);
  pat += '"';
  pat += key;
  pat += "\":\"";
  const auto pos = line.find(pat);
  if (pos == std::string_view::npos) return {};
  std::size_t curr = pos + pat.size();
  std::string out;
  while (curr < line.size()) {
    if (line[curr] == '"') break;
    if (line[curr] == '\\' && curr + 1 < line.size()) {
      char next = line[curr + 1];
      if (next == '"' || next == '\\' || next == '/') out += next;
      else if (next == 'n') out += '\n';
      else if (next == 'r') out += '\r';
      else if (next == 't') out += '\t';
      else out += next; // fallback
      curr += 2;
    } else {
      out += line[curr];
      curr += 1;
    }
  }
  return out;
}

// One-line formatted summary: HH:MM:SS.mmm  agent  event[:tool]  detail
auto formatLine(std::string_view jsonl) -> std::string {
  const auto ts = extractStr(jsonl, "ts");
  const auto agent = extractStr(jsonl, "agent");
  const auto event = extractStr(jsonl, "event");
  const auto tool = extractStr(jsonl, "tool_name");

  // ts looks like "2026-05-26T08:32:06.123Z" — slice HH:MM:SS.mmm.
  std::string ts_short = ts.size() >= 23 ? ts.substr(11, 12) : ts;

  std::string event_col = event;
  if (!tool.empty()) {
    event_col += ":";
    event_col += tool;
  }

  std::string detail;
  if (auto prompt = extractStr(jsonl, "prompt"); !prompt.empty()) {
    constexpr std::size_t kMax = 60;
    if (prompt.size() > kMax) {
      prompt.resize(kMax);
      prompt += "…";
    }
    detail = "prompt=\"" + prompt + "\"";
  }

  auto pad = [](std::string s, std::size_t w) -> std::string {
    // Pad-only: never truncate. Long names break alignment but stay
    // greppable, which matters more for a debug log than column hygiene.
    if (s.size() < w) s.append(w - s.size(), ' ');
    return s;
  };

  std::string out;
  out += pad(ts_short, 13);
  out += "  ";
  out += pad(agent, 14);
  out += "  ";
  out += pad(event_col, 22);
  if (!detail.empty()) {
    out += "  ";
    out += detail;
  }
  return out;
}

// Read whatever new lines are available in `in`, applying filters,
// printing matches. Caller is responsible for restoring stream state
// before next call (via in.clear()).
auto drainStream(std::ifstream& in, std::string_view filter_agent,
                 std::string_view since) -> void {
  std::string line;
  auto pos = in.tellg();
  while (std::getline(in, line)) {
    if (gStopEvents) return;
    if (in.eof()) {
      in.clear();
      in.seekg(pos);
      break;
    }
    pos = in.tellg();

    if (!filter_agent.empty()) {
      if (extractStr(line, "agent") != filter_agent) continue;
    }
    if (!since.empty()) {
      if (extractStr(line, "ts") < since) continue;
    }
    std::println("{}", formatLine(line));
  }
  std::fflush(stdout);
}

}  // namespace

auto subEvents(std::span<const char* const> args) -> int {
  std::string filter_agent;
  std::string since;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--agent") {
      if (++i >= args.size()) {
        std::println(stderr, "usage: bus events [--since TS] [--agent NAME]");
        return 2;
      }
      filter_agent = args[i];
    } else if (a == "--since") {
      if (++i >= args.size()) {
        std::println(stderr, "usage: bus events [--since TS] [--agent NAME]");
        return 2;
      }
      since = args[i];
    } else {
      std::println(stderr, "bus events: unknown flag \"{}\"", a);
      return 2;
    }
  }

  installInterruptHandlers(onSignalEvents);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const auto cfg = resolveConfig();
  const std::string events_log = cfg.state_dir + "/events.jsonl";

  // Make sure state dir and file exist before trying to read/watch them
  std::error_code ec;
  std::filesystem::create_directories(cfg.state_dir, ec);
  {
    std::ofstream init_file(events_log, std::ios::app);
  }

  std::ifstream in{events_log};
  if (!in) {
    std::println(stderr, "bus events: cannot open {}", events_log);
    return 1;
  }

  // Print everything currently in the file.
  drainStream(in, filter_agent, since);

  // inotify the file for IN_MODIFY; each event triggers another drain.
  const int infd = ::inotify_init1(IN_CLOEXEC);
  if (infd < 0) {
    std::println(stderr, "bus events: inotify_init1: {}", std::strerror(errno));
    return 1;
  }
  const int wd = ::inotify_add_watch(infd, events_log.c_str(), IN_MODIFY);
  if (wd < 0) {
    std::println(stderr, "bus events: add_watch: {}", std::strerror(errno));
    ::close(infd);
    return 1;
  }

  constexpr std::size_t kBufSize =
      16 * (sizeof(inotify_event) + NAME_MAX + 1);
  std::array<char, kBufSize> buf{};

  while (!gStopEvents) {
    const auto n = ::read(infd, buf.data(), buf.size());
    if (n < 0) {
      // SIGTERM interrupts the blocking read with EINTR. Re-check the
      // stop flag before re-entering — otherwise the signal-handler
      // write to the flag races with us re-blocking in read().
      if (errno == EINTR) {
        if (gStopEvents) break;
        continue;
      }
      break;
    }
    // Drain whatever new lines arrived. clear() resets eofbit so the
    // next getline picks up the appended bytes.
    in.clear();
    drainStream(in, filter_agent, since);
  }

  ::inotify_rm_watch(infd, wd);
  ::close(infd);
  return 0;
}

}  // namespace bus
