// `bus agent-bar NAME` — per-agent status strip, one row tall.
//
// Lives above the claude pane in every agent tab via the agent_tab
// layout template. Pulls state from the broker's `state` RPC once per
// tick so the bar and the monitor can never disagree — they read the
// same snapshot computed centrally. Broker is the single source of
// truth; the bar is dumb display.
//
// Drift the local-compute model used to invite (1Hz refresh skew,
// pane-scrape race, binary-drift on long-lived viewers, inbox-cursor
// race) all live in one place now, the broker. A `bin/bus` rebuild +
// broker restart suffices to roll a state-machine tweak across every
// viewer without a viewer restart.

#include "../agent_status.h"
#include "../broker.h"
#include "../json_min.h"
#include "../rpc.h"
#include "../signals.h"
#include "../sub.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopBar = 0;
auto onSignalBar(int) -> void { gStopBar = 1; }

struct Primary {
  std::string_view glyph;
  std::string_view label;
  std::string_view color;
};

auto processView(ProcessAxis p) -> Primary {
  switch (p) {
    case ProcessAxis::New:
      return {"🌱", "NEW", kBlue};
    case ProcessAxis::Starting:
      return {"🚀", "STARTING", kBlue};
    case ProcessAxis::Compacting:
      return {"🌀", "COMPACTING", kMagenta};
    case ProcessAxis::Stuck:
      return {"🪑", "BOOT_STUCK", kBrightRed};
    case ProcessAxis::Ended:
      return {"🏁", "ENDED", kDim};
    case ProcessAxis::Gone:
      return {"👻", "GONE", kDim};
    case ProcessAxis::Alive:
      return {"?", "?", kReset};
  }
  return {"?", "?", kReset};
}

auto turnView(TurnAxis t) -> Primary {
  switch (t) {
    case TurnAxis::Ready:
      return {"💤", "IDLE", kGreen};
    case TurnAxis::Working:
      return {"🔨", "WORKING", kYellow};
    case TurnAxis::Quiet:
      return {"🌙", "QUIET", kDim};
    case TurnAxis::Orchestrating:
      return {"🪐", "ORCHESTRATING", kBrightGreen};
    case TurnAxis::Stuck:
      return {"🚧", "STUCK", kRed};
    case TurnAxis::NeedsInput:
      return {"🙋", "NEEDS_INPUT", kBrightYellow};
    case TurnAxis::Compacting:
      return {"🌀", "COMPACTING", kMagenta};
    case TurnAxis::None:
      return {"?", "?", kReset};
  }
  return {"?", "?", kReset};
}

struct AgentSnap {
  bool ok{false};
  ProcessAxis process{ProcessAxis::New};
  TurnAxis turn{TurnAxis::None};
  MailAxis mail{MailAxis::None};
  TuiAxis tui{TuiAxis::Unknown};
  std::string tool;
  std::string mode;
  std::int64_t unread{0};
  std::int64_t age_ms{-1};
  bool attached{false};
};

// One RPC call per render. The broker derives axes + ancillary fields
// once and we just translate them into a frame. Returns {ok=false}
// when the broker is down or the agent isn't known — the bar then
// renders a sentinel row rather than blowing up.
auto fetchSnap(const std::string& socket_path,
               const std::string& name) -> AgentSnap {
  AgentSnap s;
  std::map<std::string, json::Value> req;
  req.insert({"op", json::Value::from("state")});
  req.insert({"agent", json::Value::from(name)});
  auto resp = rpc::call(socket_path,
                        json::Value::fromObject(std::move(req)));
  if (!resp || !resp->getOrBool("ok")) return s;
  const auto* state = resp->get("state");
  if (state == nullptr || !state->isObject()) return s;
  const auto& obj = state->asObject();
  auto it = obj.find(name);
  if (it == obj.end() || !it->second.isObject()) return s;
  const auto& entry = it->second;
  s.ok = true;
  const auto* axes = entry.get("axes");
  if (axes != nullptr && axes->isObject()) {
    s.process = processAxisFrom(axes->getOrString("process"));
    s.turn = turnAxisFrom(axes->getOrString("turn"));
    s.mail = mailAxisFrom(axes->getOrString("mail"));
    s.tui = tuiAxisFrom(axes->getOrString("tui"));
  }
  s.tool = entry.getOrString("last_tool");
  s.mode = entry.getOrString("mode");
  s.unread = entry.getOrInt("unread");
  s.age_ms = entry.getOrInt("age_ms", -1);
  s.attached = entry.getOrBool("attached");
  return s;
}

// Mail overlays on the turn axis. Two flavors, by whether the agent can
// take the mail on its own (docs/reporting-truth.md):
//   - Ready + mail → HAS_MAIL: idle at the prompt, broker about to deliver.
//   - Quiet + mail → NEEDS_NUDGE: silent, won't self-drain — a human poke
//     unsticks the queued mail. The honest replacement for a HAS_MAIL pin
//     that would otherwise hide blocked mail behind a calm-looking state.
auto pickPrimary(const AgentSnap& s) -> Primary {
  if (s.process != ProcessAxis::Alive) return processView(s.process);
  if (s.mail == MailAxis::Pending) {
    if (s.turn == TurnAxis::Ready) return {"🔔", "HAS_MAIL", kCyan};
    if (s.turn == TurnAxis::Quiet) return {"📬", "NEEDS_NUDGE", kBrightYellow};
  }
  return turnView(s.turn);
}

// Intervention reads strictly from TuiAxis::Locked. Never trips on
// Unknown — silence beats lying on a scrolled pane.
auto interventionReason(const AgentSnap& s) -> std::string {
  if (s.tui != TuiAxis::Locked) return {};
  if (s.mode.empty()) return "mode:locked";
  return std::string{"mode:"} + s.mode;
}

auto render(const std::string& name, const std::string& socket_path) -> void {
  const auto snap = fetchSnap(socket_path, name);
  std::print("{}", kCursorHome);

  if (!snap.ok) {
    // Broker unreachable or agent unknown — show a dim sentinel.
    std::print("{}{}{} {}(broker down){}", kDim, name, kReset, kDim, kReset);
    std::print("{}", kClearEol);
    std::fflush(stdout);
    return;
  }

  const auto primary = pickPrimary(snap);
  const auto reason = interventionReason(snap);
  const auto age_s = snap.age_ms >= 0 ? snap.age_ms / 1000 : -1;

  std::print("{}{}{} ", snap.attached ? kBrightGreen : kDim,
             snap.attached ? "●" : "○", kReset);
  std::print("{}{}{} ", agentColor(name), name, kReset);
  std::print("{}  {}{}{} ", primary.glyph, primary.color, primary.label,
             kReset);

  if (snap.turn == TurnAxis::Working && !snap.tool.empty()) {
    std::print("{}🔧 {}{} ", kDim, snap.tool, kReset);
  }
  std::print("{}🕒 {}{} ", kDim, formatAge(age_s), kReset);
  std::print("{}{} {}{} ", snap.mail == MailAxis::Pending ? kCyan : kDim,
             snap.mail == MailAxis::Pending ? "📬" : "📭", snap.unread,
             kReset);

  if (!reason.empty()) {
    std::print("{}⚠  {}{} ", kBrightYellow, reason, kReset);
  }

  std::print("{}", kClearEol);
  std::fflush(stdout);
}

}  // namespace

auto subAgentBar(std::span<const char* const> args) -> int {
  if (args.size() != 1) {
    std::println(stderr, "usage: bus agent-bar NAME");
    return 2;
  }
  const std::string name{args[0]};

  installInterruptHandlers(onSignalBar);

  const auto cfg = resolveConfig();
  const auto socket = cfg.socket_path;

  std::print("{}{}", kAltOn, kCursorHide);
  std::fflush(stdout);

  while (!gStopBar) {
    render(name, socket);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  std::print("{}{}", kCursorShow, kAltOff);
  std::fflush(stdout);
  return 0;
}

}  // namespace bus
