// `bus monitor [AGENT...]` — colored 1-Hz dashboard for bus agents.
//
// Calls the broker's `state` RPC once per tick. State, axes, mail, and
// pane-derived TUI fields all come from the same central snapshot the
// agent-bar uses, so the two surfaces can never disagree on what an
// agent is doing.
//
// The FOCUS column reads events.jsonl directly (in-process, tail-only)
// for the latest UserPromptSubmit body per agent — the broker doesn't
// expose prompt bodies through the state RPC, and we'd rather not
// extend the shared lib for a viewer-only signal. Read is bounded to
// the last ~64 KiB so even a multi-MB log stays cheap.

#include "../agent_status.h"
#include "../broker.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../signals.h"
#include "../sub.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <map>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopMonitor = 0;
auto onSignalMonitor(int) -> void { gStopMonitor = 1; }

// ─── data layer ──────────────────────────────────────────────────────

// Convert the wire state label back to the State enum so we can keep
// using stateName / stateColor / stateGlyph for table rendering.
auto computeStateFromLabel(std::string_view label) -> State {
  if (label == "NEW") return State::New;
  if (label == "STARTING") return State::Starting;
  if (label == "IDLE") return State::Idle;
  if (label == "HAS_MAIL") return State::HasMail;
  if (label == "WORKING") return State::Working;
  if (label == "STUCK") return State::Stuck;
  if (label == "COMPACTING") return State::Compacting;
  if (label == "NEEDS_INPUT") return State::NeedsInput;
  if (label == "BOOT_STUCK") return State::BootStuck;
  if (label == "ENDED") return State::Ended;
  if (label == "GONE") return State::Gone;
  return State::New;
}

// One RPC + a broker-liveness flag. broker_alive=false signals
// "couldn't reach the broker" — the dashboard renders a banner so a
// dead broker is visible rather than masquerading as an empty fleet.
struct Snapshot {
  bool broker_alive{false};
  json::Value state;
};

auto fetchAll(const std::string& socket_path,
              const std::set<std::string>& filter) -> Snapshot {
  Snapshot snap;
  snap.state = json::Value::fromObject({});
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("state")});
  auto resp = rpc::call(socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp || !resp->getOrBool("ok")) return snap;
  snap.broker_alive = true;
  const auto* state = resp->get("state");
  if (state == nullptr || !state->isObject()) return snap;
  if (filter.empty()) {
    snap.state = *state;
    return snap;
  }
  std::map<std::string, json::Value> out;
  for (const auto& [name, entry] : state->asObject()) {
    if (filter.contains(name)) out.insert({name, entry});
  }
  snap.state = json::Value::fromObject(std::move(out));
  return snap;
}

// Honors $CLAUDE_BUS_STATE; same fallback the rest of the code uses.
auto stateDir() -> const std::string& {
  static const std::string dir = [] {
    const char* env = std::getenv("CLAUDE_BUS_STATE");
    return std::string{env ? env : "/tmp/claude-bus"};
  }();
  return dir;
}

auto eventsLogPath() -> const std::string& {
  static const std::string path = stateDir() + "/events.jsonl";
  return path;
}

// Read $STATE/title/<name> — set by `bus msg mail --title`. The TITLE
// column's source. Empty string when no title has been set (or it was
// explicitly cleared).
auto titleFromFile(std::string_view name) -> std::string {
  std::string path = stateDir() + "/title/" + std::string{name};
  std::ifstream in{path};
  if (!in) return {};
  std::string content;
  std::getline(in, content);
  return content;
}

// Basename of a slash-separated path. Drops trailing slashes first.
// Empty input → empty output.
auto pathBasename(std::string_view path) -> std::string {
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  const auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return std::string{path};
  return std::string{path.substr(slash + 1)};
}

// Light JSON string-value extractor. Returns the raw inner-bytes between
// the matching quotes, no escape handling. Fine for fields we know are
// plain ASCII (agent, event); the prompt body needs more care — see
// extractPromptTrimmed.
auto extractStr(std::string_view line, std::string_view key) -> std::string {
  std::string pat;
  pat.reserve(key.size() + 4);
  pat += '"';
  pat += key;
  pat += "\":\"";
  const auto pos = line.find(pat);
  if (pos == std::string_view::npos) return {};
  const auto start = pos + pat.size();
  const auto end = line.find('"', start);
  if (end == std::string_view::npos) return {};
  return std::string(line.substr(start, end - start));
}

// Per-agent tail-scan result. Only `cwd` after the FOCUS column was
// dropped per sulin's review — keep the events.jsonl scan as one pass
// in case future columns want to share it.
struct AgentTail {
  std::string cwd;  // latest payload.cwd seen for any event
};

auto latestAgentTails() -> std::map<std::string, AgentTail> {
  std::map<std::string, AgentTail> out;
  std::ifstream in{eventsLogPath(), std::ios::ate | std::ios::binary};
  if (!in) return out;
  const auto end_pos = in.tellg();
  if (end_pos <= 0) return out;
  constexpr std::streamoff kTailBytes = 64 * 1024;
  const std::streamoff end_off = static_cast<std::streamoff>(end_pos);
  const std::streamoff start = std::max<std::streamoff>(0, end_off - kTailBytes);
  in.seekg(start);
  std::string line;
  if (start > 0) std::getline(in, line);  // skip partial leading line
  while (std::getline(in, line)) {
    auto agent = extractStr(line, "agent");
    if (agent.empty()) continue;
    auto cwd = extractStr(line, "cwd");
    if (!cwd.empty()) out[agent].cwd = std::move(cwd);
  }
  return out;
}

// ─── rendering ───────────────────────────────────────────────────────

constexpr std::size_t kProjectColWidth = 12;
constexpr std::size_t kTitleColWidth = 56;

// TITLE column source — $STATE/title/<agent>, set by `bus msg mail
// --title TITLE`. Truncates to the column width.
auto formatTitle(std::string_view title) -> std::string {
  if (title.empty()) return "—";
  std::string out{title};
  if (out.size() > kTitleColWidth - 1) {
    out.resize(kTitleColWidth - 2);
    out += "…";
  }
  return out;
}

// PROJECT column — basename of the cwd we saw in the agent's most
// recent event. Truncates to fit the column.
auto formatProject(std::string_view cwd) -> std::string {
  if (cwd.empty()) return "—";
  auto base = pathBasename(cwd);
  if (base.empty()) base = "/";
  if (base.size() > kProjectColWidth - 1) {
    base.resize(kProjectColWidth - 2);
    base += "…";
  }
  return base;
}

// Mail cell — bare count when zero is dim, > 0 is cyan-emphasized.
// One column instead of (glyph + count).
auto formatMail(std::int64_t unread) -> std::string {
  if (unread <= 0) return "·";
  return std::to_string(unread);
}

auto render(const Snapshot& snap, std::int64_t /*now_ms*/) -> void {
  std::print("{}", kCursorHome);

  // Title row — compact, no emoji clutter.
  std::println("{}🚌 claude-bus monitor{}   {}refresh 1s · Ctrl+C exit{}{}",
               kBold, kReset, kDim, kReset, kClearEol);

  // Broker liveness — single status row.
  if (snap.broker_alive) {
    std::println("{}🔌 broker connected{}{}", kDim, kReset, kClearEol);
  } else {
    std::println("{}⛔ broker down — retrying...{}{}",
                 kRed, kReset, kClearEol);
  }

  std::println("{}", kClearEol);

  // Header — column widths matched 1:1 to the data row below. The
  // "    " (4-space) slot stands in for the state-glyph (2 chars) +
  // its trailing space + the space after the agent name.
  std::println("{}  {:<12}    {:<10} {:>3} {:>7} {:<12} {}{}{}",
               kBold, "AGENT", "STATE", "✉", "AGE", "PROJECT", "TITLE",
               kReset, kClearEol);

  if (!snap.broker_alive) {
    std::print("{}", kClearBelow);
    std::fflush(stdout);
    return;
  }

  const auto& state = snap.state;
  if (!state.isObject() || state.asObject().empty()) {
    std::println("{}  (no live agents){}{}", kDim, kReset, kClearEol);
    std::print("{}", kClearBelow);
    std::fflush(stdout);
    return;
  }

  // Tail-scan events.jsonl once per render for FOCUS prompt + PROJECT cwd.
  const auto tails = latestAgentTails();

  std::size_t rendered = 0;
  for (const auto& [name, entry] : state.asObject()) {
    if (!entry.isObject()) continue;
    const auto state_label = entry.getOrString("state");
    if (state_label == "GONE" || state_label == "ENDED") continue;

    const auto st = computeStateFromLabel(state_label);
    const auto unread = entry.getOrInt("unread");
    const auto age_ms = entry.getOrInt("age_ms", -1);
    const auto age_s = age_ms >= 0 ? age_ms / 1000 : -1;
    const auto last_event = entry.getOrString("last_event");
    const auto last_tool = entry.getOrString("last_tool");
    const auto buffer = entry.getOrString("buffer");
    const auto attached = entry.getOrBool("attached");
    const bool has_draft = !buffer.empty() && buffer != "(empty)";

    std::string cwd;
    if (auto it = tails.find(name); it != tails.end()) {
      cwd = it->second.cwd;
    }
    const auto project = formatProject(cwd);
    const auto file_title = titleFromFile(name);
    const auto title = formatTitle(file_title);
    (void)last_event;
    (void)last_tool;
    (void)has_draft;

    // Attach dot — green when attached, dim when not.
    const auto attach_glyph = attached ? "●" : "○";
    const auto attach_color = attached ? kBrightGreen : kDim;

    const auto mail_color = unread > 0 ? kCyan : kDim;
    const auto mail_cell = formatMail(unread);

    std::println(
        "{}{}{} {}{:<12}{} {} {}{:<10}{} {}{:>3}{} {}{:>7}{} "
        "{}{:<12}{} {}{:<56}{}{}",
        attach_color, attach_glyph, kReset,
        agentColor(name), name, kReset,
        stateGlyph(st),
        stateColor(st), stateName(st), kReset,
        mail_color, mail_cell, kReset,
        kDim, formatAge(age_s), kReset,
        cwd.empty() ? kDim : "", project, kReset,
        file_title.empty() ? kDim : "", title, kReset,
        kClearEol);
    ++rendered;
  }
  if (rendered == 0) {
    std::println("{}  (no live agents){}{}", kDim, kReset, kClearEol);
  }
  std::print("{}", kClearBelow);
  std::fflush(stdout);
}

}  // namespace

auto subMonitor(std::span<const char* const> args) -> int {
  installInterruptHandlers(onSignalMonitor);

  std::set<std::string> filter;
  for (const auto* a : args) filter.insert(a);

  const auto cfg = resolveConfig();
  const auto socket = cfg.socket_path;

  std::print("{}{}", kAltOn, kCursorHide);
  std::fflush(stdout);

  while (!gStopMonitor) {
    const auto snap = fetchAll(socket, filter);
    render(snap, nowMs());
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::print("{}{}", kCursorShow, kAltOff);
  std::fflush(stdout);
  return 0;
}

}  // namespace bus
