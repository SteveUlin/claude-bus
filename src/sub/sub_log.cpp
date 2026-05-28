// `bus log [--since DUR] [--agent NAME]`
//
// Colorized lifecycle log. Tails $STATE/events.jsonl and emits one
// line per fleet-lifecycle transition — replacing the previous
// `bus topic inbox ops` viewer per docs/ops-inbox-redesign.md.
//
// Filter set (everything else dropped):
//   - SessionStart                                  → "started"
//   - UserPromptSubmit                              → "received"
//   - PreToolUse(TaskUpdate    status=in_progress) → "working"
//   - PreToolUse(TaskCreate    status=in_progress) → "working"
//   - PreToolUse(TodoWrite     has in_progress)    → "working"
//   - Stop                                          → "finished"
//   - Notification(idle_prompt)                     → "idle"
//   - Notification(permission_prompt)               → "needs input"
//   - SessionEnd                                    → "ended"
//
// "Y" (body) resolution priority for transitions that want it:
//   $STATE/title/<agent>  >  $STATE/focus/<agent>  >  inline payload
//
// v1: no audit-topic merge. The broker still writes delivery-failure
// records to inbox-ops; that's audit-as-data we'll fold into this
// view in v2 once the shape settles.

#include "../broker.h"
#include "../bus.h"
#include "../signals.h"
#include "../sub.h"
#include "../agent_status.h"

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopLog = 0;
auto onSignalLog(int) -> void { gStopLog = 1; }

// ─── parsing ────────────────────────────────────────────────────────

// "5m", "30s", "1h", "2d" → milliseconds. Returns -1 on parse failure.
auto durationToMs(std::string_view s) -> std::int64_t {
  if (s.empty()) return -1;
  const char unit = s.back();
  const auto num_sv = s.substr(0, s.size() - 1);
  std::string num{num_sv};
  const std::int64_t n = std::atoll(num.c_str());
  if (n <= 0) return -1;
  switch (unit) {
    case 's': return n * 1000;
    case 'm': return n * 60 * 1000;
    case 'h': return n * 60 * 60 * 1000;
    case 'd': return n * 24 * 60 * 60 * 1000;
    default: return -1;
  }
}

auto formatIso(std::chrono::system_clock::time_point tp) -> std::string {
  using namespace std::chrono;
  const auto secs = duration_cast<seconds>(tp.time_since_epoch()).count();
  const auto ms =
      duration_cast<milliseconds>(tp.time_since_epoch()).count() % 1000;
  std::time_t t = static_cast<std::time_t>(secs);
  std::tm tm;
  ::gmtime_r(&t, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf),
                "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<long long>(ms));
  return std::string{buf};
}

// Threshold timestamp = (now - dur_ms) formatted as the same ISO shape
// log-event.sh writes — lexicographic string comparison against the
// `ts` field works because both use 23-char UTC ISO.
auto thresholdIso(std::int64_t dur_ms) -> std::string {
  return formatIso(std::chrono::system_clock::now() -
                   std::chrono::milliseconds{dur_ms});
}

// Accept either a full 23-char UTC ISO timestamp (paste from
// events.jsonl) or a short HH:MM:SS / HH:MM:SS.mmm (paste from a
// `bus log` row). Short forms are completed with today's UTC date.
// Returns empty string on unrecognized input.
auto expandToIso(std::string_view input) -> std::string {
  if (input.size() >= 20 && input.find('T') != std::string_view::npos) {
    // Already a full ISO. Pad with ".000Z" if the user omitted the
    // millis (some sources strip them).
    std::string out{input};
    if (out.find('.') == std::string::npos &&
        out.back() == 'Z') {
      out.insert(out.size() - 1, ".000");
    }
    return out;
  }
  // Short form: 8 chars (HH:MM:SS) or 12 chars (HH:MM:SS.mmm).
  if (input.size() == 8 || input.size() == 12) {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto secs = duration_cast<seconds>(now.time_since_epoch()).count();
    std::time_t t = static_cast<std::time_t>(secs);
    std::tm tm;
    ::gmtime_r(&t, &tm);
    char date_buf[16];
    std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    std::string out{date_buf};
    out += 'T';
    out += input;
    if (input.size() == 8) out += ".000";
    out += 'Z';
    return out;
  }
  return {};
}

// at_iso + dur_ms → upper-bound ISO. Uses an integer ms parse on the
// fractional + seconds + minute + hour fields rather than re-going
// through chrono — the input shape is fixed.
auto shiftIso(std::string_view at_iso, std::int64_t dur_ms) -> std::string {
  // Parse "YYYY-MM-DDTHH:MM:SS.mmmZ".
  if (at_iso.size() < 24) return {};
  int y, mo, d, h, mi, s, ms_val;
  if (std::sscanf(std::string{at_iso}.c_str(),
                  "%d-%d-%dT%d:%d:%d.%dZ",
                  &y, &mo, &d, &h, &mi, &s, &ms_val) != 7) {
    return {};
  }
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = s;
  const std::time_t base = ::timegm(&tm);
  using namespace std::chrono;
  return formatIso(system_clock::from_time_t(base) +
                   milliseconds{ms_val + dur_ms});
}

// JSON string-value extractor with the common-escape unescape pass.
// Matches the helper in sub_events.cpp — agents' prompt bodies carry
// embedded \" and \n that we want decoded.
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
      const char next = line[curr + 1];
      if (next == '"' || next == '\\' || next == '/') out += next;
      else if (next == 'n') out += '\n';
      else if (next == 'r') out += '\r';
      else if (next == 't') out += '\t';
      else out += next;
      curr += 2;
    } else {
      out += line[curr];
      curr += 1;
    }
  }
  return out;
}

// First-line-trimmed, single-line preview of a multi-line string.
auto firstLine(std::string_view s, std::size_t max_chars = 50) -> std::string {
  auto nl = s.find('\n');
  std::string out{nl == std::string_view::npos ? s : s.substr(0, nl)};
  // Trim trailing whitespace.
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                           out.back() == '\r')) {
    out.pop_back();
  }
  if (out.size() > max_chars) {
    out.resize(max_chars - 1);
    out += "…";
  }
  return out;
}

auto stateDir() -> const std::string& {
  static const std::string dir = [] {
    const char* env = std::getenv("CLAUDE_BUS_STATE");
    return std::string{env ? env : "/tmp/claude-bus"};
  }();
  return dir;
}

auto readFileFirstLine(std::string_view path) -> std::string {
  std::ifstream in{std::string{path}};
  if (!in) return {};
  std::string content;
  std::getline(in, content);
  return content;
}

// Y resolution: title > focus > "".
auto resolveY(std::string_view agent) -> std::string {
  auto t = readFileFirstLine(stateDir() + "/title/" + std::string{agent});
  if (!t.empty()) return t;
  auto f = readFileFirstLine(stateDir() + "/focus/" + std::string{agent});
  return f;
}

// ─── line model ─────────────────────────────────────────────────────

struct LogLine {
  std::string time;        // HH:MM:SS (no millis — log is sparser than events)
  std::string agent;
  std::string glyph;
  std::string kind;        // "started" / "working" / "finished" / …
  std::string body;        // may be empty for idle/needs-input
  std::string_view color;  // line accent
  std::string_view body_color;  // dim for fallbacks
};

// Kind → glyph + color table. Surface-level constants; bunched here so
// the visual identity can be tweaked without hunting the body.
struct KindStyle {
  const char* glyph;
  std::string_view color;
};

constexpr KindStyle kStyleStart      = {"▸",  kCyan};
constexpr KindStyle kStyleReceived   = {"⊕",  kCyan};
constexpr KindStyle kStyleWorking    = {"◇",  kBlue};
constexpr KindStyle kStyleFinished   = {"✓",  kGreen};
constexpr KindStyle kStyleIdle       = {"○",  kDim};
constexpr KindStyle kStyleNeedsInput = {"❓", kYellow};
constexpr KindStyle kStyleEnded      = {"◾",  kDim};

auto sliceTime(std::string_view ts) -> std::string {
  // ISO ts "2026-05-28T15:35:08.687Z" → "15:35:08"
  if (ts.size() >= 19) return std::string{ts.substr(11, 8)};
  return std::string{ts};
}

// Build a LogLine from a JSONL row, or nullopt if filtered out.
auto buildLine(std::string_view line) -> std::optional<LogLine> {
  const auto event = extractStr(line, "event");
  if (event.empty()) return std::nullopt;
  const auto agent = extractStr(line, "agent");
  if (agent.empty()) return std::nullopt;
  const auto ts = extractStr(line, "ts");

  LogLine ll;
  ll.time = sliceTime(ts);
  ll.agent = agent;

  if (event == "SessionStart") {
    ll.kind = "started";
    ll.glyph = kStyleStart.glyph;
    ll.color = kStyleStart.color;
    const auto source = extractStr(line, "source");
    if (!source.empty()) ll.body = source;
  } else if (event == "UserPromptSubmit") {
    ll.kind = "received";
    ll.glyph = kStyleReceived.glyph;
    ll.color = kStyleReceived.color;
    const auto prompt = extractStr(line, "prompt");
    if (!prompt.empty()) ll.body = firstLine(prompt);
  } else if (event == "Stop") {
    ll.kind = "finished";
    ll.glyph = kStyleFinished.glyph;
    ll.color = kStyleFinished.color;
    ll.body_color = kDim;
    const auto last = extractStr(line, "last_assistant_message");
    if (!last.empty()) ll.body = firstLine(last);
    else ll.body = resolveY(agent);  // fall back to focus/title
  } else if (event == "Notification") {
    const auto nt = extractStr(line, "notification_type");
    if (nt == "idle_prompt") {
      ll.kind = "idle";
      ll.glyph = kStyleIdle.glyph;
      ll.color = kStyleIdle.color;
    } else if (nt == "permission_prompt") {
      ll.kind = "needs input";
      ll.glyph = kStyleNeedsInput.glyph;
      ll.color = kStyleNeedsInput.color;
    } else {
      return std::nullopt;
    }
  } else if (event == "SessionEnd") {
    ll.kind = "ended";
    ll.glyph = kStyleEnded.glyph;
    ll.color = kStyleEnded.color;
    ll.body_color = kDim;
    const auto reason = extractStr(line, "reason");
    if (!reason.empty()) ll.body = reason;
  } else if (event == "PreToolUse") {
    const auto tool = extractStr(line, "tool_name");
    if (tool != "TaskUpdate" && tool != "TaskCreate" &&
        tool != "TodoWrite") {
      return std::nullopt;
    }
    // Coarse "is this an in-progress transition?" check on the raw
    // line. Cheap; aligns with what the focus-write hook already does
    // for the file we read from below.
    if (line.find("\"status\":\"in_progress\"") == std::string::npos) {
      return std::nullopt;
    }
    ll.kind = "working";
    ll.glyph = kStyleWorking.glyph;
    ll.color = kStyleWorking.color;
    ll.body = resolveY(agent);  // title > focus
  } else {
    return std::nullopt;
  }
  return ll;
}

// ─── render ─────────────────────────────────────────────────────────

constexpr std::size_t kAgentColWidth = 12;
constexpr std::size_t kKindColWidth  = 12;

auto pad(std::string s, std::size_t w) -> std::string {
  if (s.size() < w) s.append(w - s.size(), ' ');
  return s;
}

auto printLine(const LogLine& ll) -> void {
  std::println(
      "{}{}{}  {}{}{}  {}{}{} {}{}{}{}{}{}",
      kDim, ll.time, kReset,
      agentColor(ll.agent), pad(ll.agent, kAgentColWidth), kReset,
      ll.color, ll.glyph, kReset,
      ll.color, pad(ll.kind, kKindColWidth), kReset,
      ll.body_color.empty() ? std::string_view{""} : ll.body_color,
      ll.body, kReset);
}

// ─── drain loop ─────────────────────────────────────────────────────

auto drainStream(std::ifstream& in, std::string_view filter_agent,
                 std::string_view since_iso,
                 std::string_view until_iso) -> void {
  std::string line;
  auto pos = in.tellg();
  while (std::getline(in, line)) {
    if (gStopLog) return;
    if (in.eof()) {
      in.clear();
      in.seekg(pos);
      break;
    }
    pos = in.tellg();
    if (!filter_agent.empty()) {
      if (extractStr(line, "agent") != filter_agent) continue;
    }
    const auto ts = extractStr(line, "ts");
    if (!since_iso.empty() && !ts.empty() && ts < since_iso) continue;
    if (!until_iso.empty() && !ts.empty() && ts > until_iso) continue;
    auto built = buildLine(line);
    if (!built) continue;
    printLine(*built);
  }
  std::fflush(stdout);
}

}  // namespace

auto subLog(std::span<const char* const> args) -> int {
  std::string filter_agent;
  std::string since_dur = "5m";
  std::string at_in;
  std::string window_dur;

  constexpr std::string_view kUsage =
      "usage: bus log [--since DUR] [--agent NAME] "
      "[--at TIMESTAMP] [--window DUR]";

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view a{args[i]};
    if (a == "--agent") {
      if (++i >= args.size()) {
        std::println(stderr, "{}", kUsage);
        return 2;
      }
      filter_agent = args[i];
    } else if (a == "--since") {
      if (++i >= args.size()) {
        std::println(stderr, "{}", kUsage);
        return 2;
      }
      since_dur = args[i];
    } else if (a == "--at") {
      if (++i >= args.size()) {
        std::println(stderr, "{}", kUsage);
        return 2;
      }
      at_in = args[i];
    } else if (a == "--window") {
      if (++i >= args.size()) {
        std::println(stderr, "{}", kUsage);
        return 2;
      }
      window_dur = args[i];
    } else {
      std::println(stderr, "bus log: unknown flag \"{}\"", a);
      return 2;
    }
  }

  // Postmortem mode: --at sets an explicit start timestamp instead
  // of computing one from --since. --window narrows the upper bound;
  // without --window, render through to end-of-file (no live tail).
  const bool postmortem = !at_in.empty();

  std::string since_iso;
  std::string until_iso;
  if (postmortem) {
    since_iso = expandToIso(at_in);
    if (since_iso.empty()) {
      std::println(stderr,
                   "bus log: --at wants HH:MM:SS or ISO 8601 (got \"{}\")",
                   at_in);
      return 2;
    }
    if (!window_dur.empty()) {
      const auto ms = durationToMs(window_dur);
      if (ms <= 0) {
        std::println(stderr,
                     "bus log: --window wants Ns/m/h/d (got \"{}\")",
                     window_dur);
        return 2;
      }
      until_iso = shiftIso(since_iso, ms);
    }
  } else {
    if (!window_dur.empty()) {
      std::println(stderr,
                   "bus log: --window only meaningful with --at");
      return 2;
    }
    if (!since_dur.empty()) {
      const auto ms = durationToMs(since_dur);
      if (ms <= 0) {
        std::println(stderr,
                     "bus log: --since wants Ns/m/h/d (got \"{}\")",
                     since_dur);
        return 2;
      }
      since_iso = thresholdIso(ms);
    }
  }

  installInterruptHandlers(onSignalLog);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  const auto cfg = resolveConfig();
  const std::string events_log = cfg.state_dir + "/events.jsonl";

  std::error_code ec;
  std::filesystem::create_directories(cfg.state_dir, ec);
  { std::ofstream init_file(events_log, std::ios::app); }

  std::ifstream in{events_log};
  if (!in) {
    std::println(stderr, "bus log: cannot open {}", events_log);
    return 1;
  }

  // Header banner — minimal; the rows speak for themselves.
  if (postmortem) {
    std::println("{}🚌 bus log{}  {}replay from {}{}{}{}",
                 kBold, kReset, kDim, since_iso,
                 until_iso.empty() ? "" : std::format(" through {}",
                                                       until_iso).c_str(),
                 kReset, "");
  } else {
    std::println("{}🚌 bus log{}  {}since {} · Ctrl+C exit{}",
                 kBold, kReset, kDim, since_dur, kReset);
  }

  drainStream(in, filter_agent, since_iso, until_iso);

  if (postmortem) {
    // Postmortem: render the slice and exit. No inotify, no waiting
    // for new events to arrive after the window ends.
    return 0;
  }

  const int infd = ::inotify_init1(IN_CLOEXEC);
  if (infd < 0) {
    std::println(stderr, "bus log: inotify_init1: {}", std::strerror(errno));
    return 1;
  }
  const int wd = ::inotify_add_watch(infd, events_log.c_str(), IN_MODIFY);
  if (wd < 0) {
    std::println(stderr, "bus log: add_watch: {}", std::strerror(errno));
    ::close(infd);
    return 1;
  }

  constexpr std::size_t kBufSize =
      16 * (sizeof(inotify_event) + NAME_MAX + 1);
  std::array<char, kBufSize> buf{};

  while (!gStopLog) {
    const auto n = ::read(infd, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) {
        if (gStopLog) break;
        continue;
      }
      break;
    }
    in.clear();
    drainStream(in, filter_agent, since_iso, until_iso);
  }

  ::inotify_rm_watch(infd, wd);
  ::close(infd);
  return 0;
}

}  // namespace bus
