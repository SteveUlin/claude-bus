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
#include <vector>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopDeck = 0;
auto onSignalDeck(int) -> void { gStopDeck = 1; }

// ─── helpers ────────────────────────────────────────────────────────

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
      const char n = line[curr + 1];
      if (n == '"' || n == '\\' || n == '/') out += n;
      else if (n == 'n') out += '\n';
      else if (n == 'r') out += '\r';
      else if (n == 't') out += '\t';
      else out += n;
      curr += 2;
    } else {
      out += line[curr];
      ++curr;
    }
  }
  return out;
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

// Pull `context_window.used_percentage` from $STATE/status/<agent>.json
// if the statusline sidecar (docs/context-budget.md) is writing it.
// Returns -1 when unavailable so the renderer can fall back to "—".
auto contextPctFor(std::string_view agent) -> int {
  const std::string path =
      stateDir() + "/status/" + std::string{agent} + ".json";
  std::ifstream in{path};
  if (!in) return -1;
  std::string content{std::istreambuf_iterator<char>{in}, {}};
  if (content.empty()) return -1;
  auto parsed = json::parse(content);
  if (!parsed || !parsed->isObject()) return -1;
  const auto* cw = parsed->get("context_window");
  if (cw == nullptr || !cw->isObject()) return -1;
  const auto pct = cw->getOrInt("used_percentage", -1);
  return pct > 100 ? 100 : static_cast<int>(pct);
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
    auto agent = extractStr(line, "agent");
    if (agent.empty()) continue;
    auto& slot = out[agent];
    auto cwd = extractStr(line, "cwd");
    if (!cwd.empty()) slot.cwd = std::move(cwd);
    if (line.find("\"notification_type\":\"permission_prompt\"") !=
        std::string::npos) {
      auto msg = extractStr(line, "message");
      if (!msg.empty()) slot.ask = std::move(msg);
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
  r.context_pct = contextPctFor(name);

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

auto formatMeta(std::int64_t age_s, int ctx_pct) -> std::string {
  const auto age = formatAgeBrief(age_s);
  if (ctx_pct < 0) return std::format("({} · —)", age);
  return std::format("({} · {}%)", age, ctx_pct);
}

auto verbFor(State s) -> std::string_view {
  switch (s) {
    case State::Working:     return "working on";
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
  const auto meta = formatMeta(r.age_s, r.context_pct);
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
