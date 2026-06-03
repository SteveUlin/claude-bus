// `bus deck` — per-agent one-sentence-per-row activity card.
//
// sulin's framing: "I want it to be a thing I can reference to know,
// ok, agent x is working on testing now, or agent y is running a
// sim." This is a reference card, not a transcript. One sentence per
// agent. ⚠ NEEDS YOU pinned at the top. ✓ FLEET sorted WORKING →
// IDLE → STARTING. GONE/ENDED hidden. See docs/comms-ui.md.
//
// v1 leaves the context-% slot showing "—". The statusline sidecar
// that would populate $STATE/status/<agent>.json (per
// docs/context-budget.md) isn't built yet; when it lands, deck will
// pick it up without further changes here — the column renderer
// reads the file if present.

#include "../agent_status.h"
#include "../broker.h"
#include "../event.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../signals.h"
#include "../state_paths.h"
#include "../sub.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ios>
#include <sstream>
#include <map>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopDeck = 0;
auto onSignalDeck(int) -> void { gStopDeck = 1; }

// ─── helpers ────────────────────────────────────────────────────────

auto stateDir() -> const std::string& {
  static const std::string dir = [] { return bus::stateRoot(); }();
  return dir;
}

auto eventsLogPath() -> const std::string& {
  static const std::string path = stateDir() + "/events.jsonl";
  return path;
}

auto computeStateFromLabel(std::string_view label) -> State {
  if (label == "NEW") return State::New;
  if (label == "STARTING") return State::Starting;
  if (label == "IDLE") return State::Idle;
  if (label == "HAS_MAIL") return State::HasMail;
  if (label == "WORKING") return State::Working;
  if (label == "ORCHESTRATING") return State::Orchestrating;
  if (label == "STUCK") return State::Stuck;
  if (label == "COMPACTING") return State::Compacting;
  if (label == "NEEDS_INPUT") return State::NeedsInput;
  if (label == "BOOT_STUCK") return State::BootStuck;
  if (label == "ENDED") return State::Ended;
  if (label == "GONE") return State::Gone;
  return State::New;
}

auto readFileFirstLine(std::string_view path) -> std::string {
  std::ifstream in{std::string{path}};
  if (!in) return {};
  std::string content;
  std::getline(in, content);
  return content;
}

auto titleFor(std::string_view agent) -> std::string {
  auto t = readFileFirstLine(stateDir() + "/title/" + std::string{agent});
  if (!t.empty()) return t;
  return readFileFirstLine(stateDir() + "/focus/" + std::string{agent});
}

// Scan `"<key>": <int>` after `"<scope>"` opens. Returns -1 on miss.
// Avoids json::parse — the statusline payload contains
// `cost.total_cost_usd` as a float and our json_min doesn't speak
// floats. The file shape is stable enough to scan.
auto scanIntAfter(std::string_view content,
                  std::string_view scope_key,
                  std::string_view leaf_key) -> long long {
  auto scope = content.find(scope_key);
  if (scope == std::string_view::npos) return -1;
  auto key = content.find(leaf_key, scope);
  if (key == std::string_view::npos) return -1;
  auto i = content.find(':', key);
  if (i == std::string_view::npos) return -1;
  ++i;
  while (i < content.size() && (content[i] == ' ' || content[i] == '\t')) ++i;
  long long n = 0;
  bool seen = false;
  while (i < content.size() && content[i] >= '0' && content[i] <= '9') {
    n = n * 10 + (content[i] - '0');
    seen = true;
    ++i;
  }
  return seen ? n : -1;
}

struct CtxStats {
  int pct{-1};                    // used_percentage
  long long size_tokens{-1};      // context_window_size
};

auto contextStatsFor(std::string_view agent) -> CtxStats {
  CtxStats out;
  const std::string path =
      stateDir() + "/status/" + std::string{agent} + ".json";
  std::ifstream in{path};
  if (!in) return out;
  std::ostringstream buf;
  buf << in.rdbuf();
  const auto content = buf.str();
  const auto pct = scanIntAfter(content, "\"context_window\"",
                                "\"used_percentage\"");
  if (pct >= 0) out.pct = static_cast<int>(pct > 100 ? 100 : pct);
  out.size_tokens = scanIntAfter(content, "\"context_window\"",
                                 "\"context_window_size\"");
  return out;
}

// Compact ceiling label: 1000000 → "1M", 200000 → "200K", else "<N>".
auto formatCtxSize(long long tokens) -> std::string {
  if (tokens <= 0) return "?";
  if (tokens >= 1'000'000 && tokens % 1'000'000 == 0) {
    return std::format("{}M", tokens / 1'000'000);
  }
  if (tokens >= 1000 && tokens % 1000 == 0) {
    return std::format("{}K", tokens / 1000);
  }
  return std::format("{}", tokens);
}

auto firstLine(std::string_view s, std::size_t max_chars = 56) -> std::string {
  auto nl = s.find('\n');
  std::string out{nl == std::string_view::npos ? s : s.substr(0, nl)};
  while (!out.empty() &&
         (out.back() == ' ' || out.back() == '\t' || out.back() == '\r')) {
    out.pop_back();
  }
  if (out.size() > max_chars) {
    out.resize(max_chars - 1);
    out += "…";
  }
  return out;
}

// Per-agent latest signals scraped from the events.jsonl tail.
struct AgentTail {
  std::string cwd;          // payload.cwd of latest event
  std::string ask;          // Notification(permission_prompt).message
};

auto latestTails() -> std::map<std::string, AgentTail> {
  std::map<std::string, AgentTail> out;
  std::ifstream in{eventsLogPath(), std::ios::ate | std::ios::binary};
  if (!in) return out;
  const auto end_pos = in.tellg();
  if (end_pos <= 0) return out;
  constexpr std::streamoff kTailBytes = 64 * 1024;
  const std::streamoff end_off = static_cast<std::streamoff>(end_pos);
  const std::streamoff start =
      std::max<std::streamoff>(0, end_off - kTailBytes);
  in.seekg(start);
  std::string line;
  if (start > 0) std::getline(in, line);
  while (std::getline(in, line)) {
    auto ev = parseEvent(line);
    if (!ev || ev->agent.empty()) continue;
    auto& slot = out[ev->agent];
    if (!ev->cwd.empty()) slot.cwd = std::move(ev->cwd);
    if (ev->notification_type == "permission_prompt" && !ev->message.empty()) {
      slot.ask = std::move(ev->message);
    }
  }
  return out;
}

// ─── data + render model ────────────────────────────────────────────

struct Snapshot {
  bool broker_alive{false};
  json::Value state;
};

auto fetchAll(const std::string& socket_path) -> Snapshot {
  Snapshot snap;
  snap.state = json::Value::fromObject({});
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("state")});
  auto resp = rpc::call(socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp || !resp->getOrBool("ok")) return snap;
  snap.broker_alive = true;
  if (const auto* s = resp->get("state"); s && s->isObject()) {
    snap.state = *s;
  }
  return snap;
}

struct Row {
  std::string name;
  State state{State::New};
  std::string subject;     // title-or-derived; ask body for NEEDS_INPUT
  std::int64_t age_s{-1};
  int context_pct{-1};
  long long context_size{-1};
};

// Build the activity sentence-body for one agent.
auto makeRow(std::string_view name, const json::Value& entry,
             const std::map<std::string, AgentTail>& tails) -> Row {
  Row r;
  r.name = name;
  const auto state_label = entry.getOrString("state");
  r.state = computeStateFromLabel(state_label);
  const auto age_ms = entry.getOrInt("age_ms", -1);
  r.age_s = age_ms >= 0 ? age_ms / 1000 : -1;
  const auto ctx = contextStatsFor(name);
  r.context_pct = ctx.pct;
  r.context_size = ctx.size_tokens;

  const auto title = titleFor(name);
  std::string ask;
  if (auto it = tails.find(std::string{name}); it != tails.end()) {
    ask = it->second.ask;
  }
  switch (r.state) {
    case State::NeedsInput:
      r.subject = !ask.empty() ? firstLine(ask) : firstLine(title);
      if (r.subject.empty()) r.subject = "needs input";
      break;
    case State::Working:
    case State::Orchestrating:
    case State::HasMail:
    case State::Compacting:
      r.subject = firstLine(title);
      break;
    case State::Stuck:
    case State::BootStuck:
      r.subject = firstLine(title);
      if (r.subject.empty()) r.subject = entry.getOrString("last_event");
      break;
    case State::Starting:
    case State::Idle:
      // verb carries the meaning; leave subject empty so we don't
      // print "kvothe idle idle".
      r.subject = "";
      break;
    default:
      r.subject = "";
  }
  return r;
}

// ─── render ─────────────────────────────────────────────────────────

constexpr std::size_t kNameColWidth = 10;
constexpr std::size_t kVerbColWidth = 10;

auto formatAgeBrief(std::int64_t age_s) -> std::string {
  if (age_s < 0) return "?";
  if (age_s < 60) return std::format("{}s", age_s);
  if (age_s < 3600) return std::format("{}m", age_s / 60);
  return std::format("{}h", age_s / 3600);
}

auto formatMeta(std::int64_t age_s, int ctx_pct,
                long long ctx_size_tokens) -> std::string {
  const auto age = formatAgeBrief(age_s);
  if (ctx_pct < 0) return std::format("({} · —)", age);
  return std::format("({} · {}%/{})", age, ctx_pct,
                     formatCtxSize(ctx_size_tokens));
}

auto verbFor(State s) -> std::string_view {
  switch (s) {
    case State::Working:     return "working on";
    case State::Orchestrating: return "orchestrating";
    case State::HasMail:     return "has mail";
    case State::NeedsInput:  return "asking";
    case State::Idle:        return "idle";
    case State::Starting:    return "starting";
    case State::Stuck:       return "stuck";
    case State::BootStuck:   return "boot-stuck";
    case State::Compacting:  return "compacting";
    default:                 return "";
  }
}

auto pad(std::string s, std::size_t w) -> std::string {
  if (s.size() < w) s.append(w - s.size(), ' ');
  return s;
}

// One per-agent line. The leading glyph is the zone marker — supplied
// by the caller so the NEEDS YOU block keeps its ⚠ even on row 2+.
auto printRow(const Row& r, std::string_view zone_glyph,
              std::string_view zone_color) -> void {
  const auto verb = std::string{verbFor(r.state)};
  const auto subject =
      (r.state == State::NeedsInput && !r.subject.empty())
          ? std::format("\"{}\"", r.subject)
          : r.subject;
  const auto meta = formatMeta(r.age_s, r.context_pct, r.context_size);
  const auto name_color = agentColor(r.name);

  std::println("{}{}{}  {}{}{} {}{}{} {}  {}{}{}{}",
               zone_color, zone_glyph, kReset,
               name_color, pad(r.name, kNameColWidth), kReset,
               kDim, pad(verb, kVerbColWidth), kReset,
               subject,
               kDim, meta, kReset, kClearEol);
}

auto render(const Snapshot& snap) -> void {
  std::print("{}", kCursorHome);

  // Header
  if (!snap.broker_alive) {
    std::println("{}⛔ broker down — retrying...{}{}", kRed, kReset, kClearEol);
    std::print("{}", kClearBelow);
    std::fflush(stdout);
    return;
  }

  // Build rows.
  const auto tails = latestTails();
  std::vector<Row> needs;
  std::vector<Row> working;
  std::vector<Row> rest;
  for (const auto& [name, entry] : snap.state.asObject()) {
    if (!entry.isObject()) continue;
    const auto state_label = entry.getOrString("state");
    if (state_label == "GONE" || state_label == "ENDED") continue;
    auto row = makeRow(name, entry, tails);
    if (row.state == State::NeedsInput) needs.push_back(std::move(row));
    else if (row.state == State::Working ||
             row.state == State::HasMail ||
             row.state == State::Compacting) {
      working.push_back(std::move(row));
    } else {
      rest.push_back(std::move(row));
    }
  }
  std::sort(needs.begin(), needs.end(),
            [](const Row& a, const Row& b) { return a.age_s > b.age_s; });
  std::sort(working.begin(), working.end(),
            [](const Row& a, const Row& b) { return a.age_s < b.age_s; });
  std::sort(rest.begin(), rest.end(),
            [](const Row& a, const Row& b) { return a.age_s < b.age_s; });

  const auto total = needs.size() + working.size() + rest.size();
  std::println("{}🚌 claude-bus{}  {}· {} agents{}{}{}",
               kBold, kReset, kDim,
               total,
               needs.empty()
                   ? std::string{}
                   : std::format(" · {} needs you", needs.size()),
               kReset, kClearEol);
  std::println("{}", kClearEol);

  if (!needs.empty()) {
    std::println("{}⚠ NEEDS YOU{}{}", kYellow, kReset, kClearEol);
    for (const auto& r : needs) printRow(r, " ", "");
    std::println("{}", kClearEol);
  }

  if (!working.empty() || !rest.empty()) {
    std::println("{}✓ FLEET{}{}", kGreen, kReset, kClearEol);
    for (const auto& r : working) printRow(r, " ", "");
    for (const auto& r : rest) printRow(r, " ", "");
  }

  std::print("{}", kClearBelow);
  std::fflush(stdout);
}

}  // namespace

auto subDeck(std::span<const char* const> args) -> int {
  if (!args.empty()) {
    std::println(stderr, "usage: bus deck");
    return 2;
  }
  installInterruptHandlers(onSignalDeck);

  const auto cfg = resolveConfig();
  const auto socket = cfg.socket_path;

  std::print("{}{}", kAltOn, kCursorHide);
  std::fflush(stdout);

  while (!gStopDeck) {
    const auto snap = fetchAll(socket);
    render(snap);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::print("{}{}", kCursorShow, kAltOff);
  std::fflush(stdout);
  return 0;
}

}  // namespace bus
