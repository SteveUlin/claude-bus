// `bus monitor [AGENT...]` — colored 1-Hz dashboard for bus agents.
//
// Calls the broker's `state` RPC once per tick. State, axes, mail, and
// pane-derived TUI fields all come from the same central snapshot the
// agent-bar uses, so the two surfaces can never disagree on what an
// agent is doing.
//
// The LANE column resolves each agent's domain tag from its role file's
// `lane:` frontmatter — the registry entry ($STATE/agents/<name>.json)
// carries the role name + workspace, from which we locate <repo>/roles.
// The TASK column reads $STATE/title/<agent> (set by `bus msg mail
// --title`). Both are read in-process from files, like CTX.

#include "../agent_status.h"
#include "../broker.h"
#include "../context_stats.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../signals.h"
#include "../state_paths.h"
#include "../sub.h"
#include "../trigger_feed.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

// One RPC + a broker-liveness flag. broker_alive=false signals
// "couldn't reach the broker" — the dashboard renders a banner so a
// dead broker is visible rather than masquerading as an empty fleet.
struct Snapshot {
  bool broker_alive{false};
  json::Value state;
  // Raw resume instant from the broker's loop-plane Δwall−Δmono detector
  // (top-level on the state RPC; 0 = no jump this broker run). Drives the
  // post-resume reconciliation banner. See docs/reporting-truth.md (a).
  std::int64_t continuity_since_ms{0};
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
  snap.continuity_since_ms = resp->getOrInt("continuity_since_ms", 0);
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
  static const std::string dir = [] { return bus::stateRoot(); }();
  return dir;
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

// Light JSON string-value extractor. Returns the raw inner-bytes between
// the matching quotes, no escape handling. Tolerates whitespace around
// the colon, so it reads both compact (events.jsonl) and pretty-printed
// (the jq-written registry) JSON. Fine for plain-ASCII fields.
auto extractStr(std::string_view line, std::string_view key) -> std::string {
  std::string pat;
  pat.reserve(key.size() + 2);
  pat += '"';
  pat += key;
  pat += '"';
  const auto kpos = line.find(pat);
  if (kpos == std::string_view::npos) return {};
  auto i = kpos + pat.size();
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] != ':') return {};
  ++i;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  if (i >= line.size() || line[i] != '"') return {};
  const auto start = i + 1;
  const auto end = line.find('"', start);
  if (end == std::string_view::npos) return {};
  return std::string(line.substr(start, end - start));
}

// ─── rendering ───────────────────────────────────────────────────────

constexpr std::size_t kLaneColWidth = 10;
constexpr std::size_t kTitleColWidth = 56;

// LANE column — the agent's domain tag. Truncates to the column width.
auto formatLane(std::string_view lane) -> std::string {
  if (lane.empty()) return "—";
  std::string out{lane};
  if (out.size() > kLaneColWidth - 1) {
    out.resize(kLaneColWidth - 2);
    out += "…";
  }
  return out;
}

// Resolve an agent's LANE (domain tag) from its role file's `lane:`
// frontmatter. The registry entry ($STATE/agents/<name>.json) carries
// the role name + workspace path; the repo root is the workspace minus
// its `/.workspaces/<name>` suffix, and roles live at <repo>/roles.
// Empty when the registry entry, role file, or `lane:` line is absent.
auto laneFromRegistry(std::string_view name) -> std::string {
  const std::string reg =
      stateDir() + "/agents/" + std::string{name} + ".json";
  std::ifstream in{reg};
  if (!in) return {};
  std::ostringstream buf;
  buf << in.rdbuf();
  const auto content = buf.str();
  const auto role = extractStr(content, "role");
  const auto workspace = extractStr(content, "workspace");
  if (role.empty() || workspace.empty()) return {};
  const auto ws_pos = workspace.find("/.workspaces/");
  const std::string repo = ws_pos == std::string::npos
                               ? workspace
                               : workspace.substr(0, ws_pos);
  std::ifstream rf{repo + "/roles/" + role + ".md"};
  if (!rf) return {};
  std::string line;
  bool in_fm = false;
  while (std::getline(rf, line)) {
    if (line == "---") {
      if (!in_fm) {
        in_fm = true;
        continue;
      }
      break;  // end of frontmatter
    }
    if (in_fm && line.starts_with("lane:")) {
      std::string_view v{line};
      v.remove_prefix(5);
      while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
        v.remove_prefix(1);
      }
      while (!v.empty() && (v.back() == ' ' || v.back() == '\t' ||
                            v.back() == '\r')) {
        v.remove_suffix(1);
      }
      return std::string{v};
    }
  }
  return {};
}

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

// Mail cell — bare count when zero is dim, > 0 is cyan-emphasized.
// One column instead of (glyph + count).
auto formatMail(std::int64_t unread) -> std::string {
  if (unread <= 0) return "·";
  return std::to_string(unread);
}

// MODEL column — abbreviates the status JSON's model string (strips the
// "claude-" vendor prefix: "claude-opus-4-8" → "opus-4-8") so the
// model+ctx pair reads at a glance. "—" until the broker's token-scan
// watcher stamps "model" into $STATE/status (it already parses the
// transcript turn the model rides on; see token-monitor handoff).
constexpr std::size_t kModelColWidth = 10;
auto formatModel(std::string_view model) -> std::string {
  if (model.empty()) return "—";
  std::string_view m = model;
  if (m.starts_with("claude-")) m.remove_prefix(7);
  std::string out{m};
  if (out.size() > kModelColWidth - 1) {
    out.resize(kModelColWidth - 2);
    out += "…";
  }
  return out;
}

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

// CTX column — '<pct>%/<size>' or '—', with pct + size both against the
// REAL per-model window. Compact: '5%/1M', '94%/200K'.
auto formatCtx(const CtxStats& s) -> std::string {
  if (s.pct < 0) return "—";
  return std::format("{}%/{}", s.pct, formatCtxSize(s.size_tokens));
}

// The behavioral compact policy: agents self-compact past ~200k tokens
// regardless of the real (1M) window. Rendered as the CTX color marker —
// auri's mandate: 200k is a policy TICK, not the denominator. Keeping it
// token-based (not %) keeps "past the compact line" salient even when 200k
// is only ~20% of a 1M window.
constexpr long long kPolicyTokens = 200'000;

// CTX cell color. Token-count policy marker on the authoritative path;
// falls back to pct tiers when only a % is known (broker fallback).
auto ctxColorFor(const CtxStats& s) -> std::string_view {
  if (s.tokens >= 0) {
    if (s.tokens >= kPolicyTokens) return kRed;
    if (s.tokens >= kPolicyTokens * 85 / 100) return kYellow;
    return kDim;
  }
  if (s.pct < 0) return kDim;
  if (s.pct >= 90) return kRed;
  if (s.pct >= 75) return kYellow;
  return kDim;
}

// EFFORT column — the live reasoning level from .effort.level (a real
// statusline-stdin field that reflects /effort changes; authoritative per
// sulin). '—' means simply not-yet-captured (pre-relaunch) or a model that
// doesn't support effort — not a harness gap, and never the launch flag.
auto formatEffort(std::string_view effort) -> std::string {
  if (effort.empty()) return "—";
  return std::string{effort};
}

auto render(const Snapshot& snap, std::int64_t now_ms) -> void {
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

  // Post-resume reconciliation banner (reporting-truth a). Within the
  // settling window after a reboot/suspend, event ages span the slept-through
  // gap — the per-agent states are already clamped honest broker-side, but
  // the AGE column still shows the inflated number. Say so, so a big AGE next
  // to a calm state reads as "settling," not "stuck." Floor = the same
  // max(systemBootMs, continuity) the broker clamps to; show it only while
  // recent (one stale-budget window), then it fades on its own.
  if (snap.broker_alive) {
    const auto boot = systemBootMs();
    const auto floor =
        snap.continuity_since_ms > boot ? snap.continuity_since_ms : boot;
    const auto since_s = floor > 0 ? (now_ms - floor) / 1000 : -1;
    if (since_s >= 0 && since_s < 5 * 60) {
      const bool jump = snap.continuity_since_ms > boot;
      std::println("{}🕒 fleet {} {} ago — event ages settling, states "
                   "reconciled{}{}",
                   kYellow, jump ? "resumed (clock jump)" : "booted",
                   formatAge(since_s), kReset, kClearEol);
    }
  }

  std::println("{}", kClearEol);

  // Header — column widths matched 1:1 to the data row below. The first
  // "  " is the alarm gutter (P3 REC marker + space); the next "  " stands
  // in for the attach dot + its trailing space.
  std::println(
      "{}    {:<12}    {:<10} {:>3} {:>7} {:<10} {:<6} {:>9} {:<10} {}{}{}",
      kBold, "AGENT", "STATE", "✉", "AGE", "MODEL", "EFFORT", "CTX", "LANE",
      "TASK", kReset, kClearEol);

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

  std::size_t rendered = 0;
  int act_count = 0;
  int wait_count = 0;
  for (const auto& [name, entry] : state.asObject()) {
    if (!entry.isObject()) continue;
    const auto state_label = entry.getOrString("state");
    if (state_label == "GONE" || state_label == "ENDED") continue;

    const auto st = bus::stateFrom(state_label);

    // P3 trigger feed — same derivation `bus triggers` uses. Refresh the
    // $STATE/triggers/<agent>.json file every tick so elodin's decoupled
    // actuator never reads a stale signal (the monitor IS the continuous
    // writer), and drive the alarm gutter below.
    const Trig trig = deriveTrig(name, entry, now_ms);
    writeTrigger(trig, now_ms);
    const Rec rec = recOf(trig);
    if (rec == Rec::Act) ++act_count;
    if (rec == Rec::Wait) ++wait_count;
    // Alarm gutter — a shape-family marker that pops in peripheral vision
    // without recoloring the per-cell signals: filled red ▲ = ACT (urgent
    // + safe boundary, intervene now), hollow yellow △ = WAIT (urgent but
    // mid-turn, hold), blank = OK.
    const char* alarm_glyph = rec == Rec::Act    ? "▲"
                              : rec == Rec::Wait ? "△"
                                                 : " ";
    const auto alarm_color = rec == Rec::Act    ? kBrightRed
                             : rec == Rec::Wait ? kYellow
                                                : kDim;
    const auto unread = entry.getOrInt("unread");
    const auto age_ms = entry.getOrInt("age_ms", -1);
    const auto age_s = age_ms >= 0 ? age_ms / 1000 : -1;
    const auto last_event = entry.getOrString("last_event");
    const auto last_tool = entry.getOrString("last_tool");
    const auto buffer = entry.getOrString("buffer");
    const auto attached = entry.getOrBool("attached");
    const bool has_draft = !buffer.empty() && buffer != "(empty)";

    const auto lane_raw = laneFromRegistry(name);
    const auto lane = formatLane(lane_raw);
    const auto file_title = titleFromFile(name);
    const auto title = formatTitle(file_title);
    const auto ctx_stats = contextStatsFor(stateDir(), name);
    const auto ctx_cell = formatCtx(ctx_stats);
    const auto model_cell = formatModel(ctx_stats.model);
    const auto effort_cell = formatEffort(ctx_stats.effort);
    // CTX color marks the 200k compact policy (token-based) on the real
    // window; falls back to pct tiers when only a % is known.
    const auto ctx_color = ctxColorFor(ctx_stats);
    (void)last_event;
    (void)last_tool;
    (void)has_draft;

    // Attach dot — green when attached, dim when not.
    const auto attach_glyph = attached ? "●" : "○";
    const auto attach_color = attached ? kBrightGreen : kDim;

    const auto mail_color = unread > 0 ? kCyan : kDim;
    const auto mail_cell = formatMail(unread);

    std::println(
        "{}{}{} {}{}{} {}{:<12}{} {} {}{:<10}{} {}{:>3}{} {}{:>7}{} "
        "{}{:<10}{} {}{:<6}{} {}{:>9}{} {}{:<10}{} {}{:<56}{}{}",
        alarm_color, alarm_glyph, kReset,
        attach_color, attach_glyph, kReset,
        agentColor(name), name, kReset,
        stateGlyph(st),
        stateColor(st), stateName(st), kReset,
        mail_color, mail_cell, kReset,
        kDim, formatAge(age_s), kReset,
        ctx_stats.model.empty() ? kDim : "", model_cell, kReset,
        ctx_stats.effort.empty() ? kDim : "", effort_cell, kReset,
        ctx_color, ctx_cell, kReset,
        lane_raw.empty() ? kDim : "", lane, kReset,
        file_title.empty() ? kDim : "", title, kReset,
        kClearEol);
    ++rendered;
  }
  if (rendered == 0) {
    std::println("{}  (no live agents){}{}", kDim, kReset, kClearEol);
  }
  // Alarm legend — only when the gutter has something to explain, so a
  // calm fleet stays uncluttered.
  if (act_count + wait_count > 0) {
    std::println("{}", kClearEol);
    std::println(
        "  {}▲ {} ACT{} urgent + safe boundary, intervene now   "
        "{}△ {} WAIT{} urgent but mid-turn, hold{}",
        kBrightRed, act_count, kReset, kYellow, wait_count, kReset,
        kClearEol);
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
