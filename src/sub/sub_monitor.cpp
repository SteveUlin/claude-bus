// `bus monitor [AGENT...]` — colored 1-Hz dashboard for bus agents.
//
// Calls the broker's `state` RPC once per tick. State, axes, mail, and
// pane-derived TUI fields all come from the same central snapshot the
// agent-bar uses, so the two surfaces can never disagree on what an
// agent is doing.

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
#include <set>
#include <span>
#include <string>
#include <thread>

namespace bus {

namespace {

using namespace bus::ansi;

volatile std::sig_atomic_t gStopMonitor = 0;
auto onSignalMonitor(int) -> void { gStopMonitor = 1; }

auto formatMode(std::string_view mode) -> std::string {
  if (mode == "INSERT" || mode == "unknown" || mode.empty()) return "";
  return std::string{mode};
}

constexpr std::size_t kInputColWidth = 28;
auto formatInput(std::string_view buffer) -> std::string {
  if (buffer.empty() || buffer == "(empty)") return "·";
  if (buffer.size() > kInputColWidth) {
    return std::string{buffer.substr(0, kInputColWidth - 2)} + "…";
  }
  return std::string{buffer};
}

// Convert the wire state label back to the State enum so we can keep
// using stateName / stateColor / stateGlyph for table rendering. The
// label set matches stateName 1:1.
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
// Mirrors the (broker down) sentinel that sub_agent_bar shows per pane.
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
  // No "agent" key when filter is empty — broker returns all agents.
  // With a single named agent, we batch one call per filter member;
  // the typical case (filter empty) needs only one round-trip.
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

auto render(const Snapshot& snap, std::int64_t /*now_ms*/) -> void {
  std::print("{}", kCursorHome);
  std::println("{}🚌 claude-bus monitor{}   {}refresh: 1s   Ctrl+C to exit{}{}",
               kBold, kReset, kDim, kReset, kClearEol);

  // Broker-liveness banner — matches the (broker down) sentinel that
  // sub_agent_bar uses per pane. The RPC retries every tick, so a
  // recovered broker flips this back to connected automatically.
  if (snap.broker_alive) {
    std::println("{}🔌 broker connected{}{}", kDim, kReset, kClearEol);
  } else {
    std::println("{}⛔ broker down — retrying...{}{}", kRed, kReset, kClearEol);
  }

  std::println("{}", kClearEol);
  std::println("{}{:<14}     {:<10} {:>5}  {:<22} {:>7}  {:<6} {:<28}{}{}",
               kBold, "AGENT", "STATE", "MAIL", "LAST EVENT", "AGE", "MODE",
               "INPUT", kReset, kClearEol);

  if (!snap.broker_alive) {
    // Don't pretend the fleet is empty; the broker just isn't answering.
    std::print("{}", kClearBelow);
    std::fflush(stdout);
    return;
  }

  const auto& state = snap.state;
  if (!state.isObject() || state.asObject().empty()) {
    std::println("{}(no live agents){}{}", kDim, kReset, kClearEol);
    std::print("{}", kClearBelow);
    std::fflush(stdout);
    return;
  }

  std::size_t rendered = 0;
  for (const auto& [name, entry] : state.asObject()) {
    if (!entry.isObject()) continue;
    const auto state_label = entry.getOrString("state");
    if (state_label == "GONE" || state_label == "ENDED") continue;

    const auto process =
        processAxisFrom(entry.get("axes") != nullptr
                            ? entry.get("axes")->getOrString("process")
                            : "");
    const auto turn =
        turnAxisFrom(entry.get("axes") != nullptr
                         ? entry.get("axes")->getOrString("turn")
                         : "");
    const auto st = computeStateFromLabel(state_label);  // see helper below
    const auto unread = entry.getOrInt("unread");
    const auto age_ms = entry.getOrInt("age_ms", -1);
    const auto age_s = age_ms >= 0 ? age_ms / 1000 : -1;
    const auto last_event = entry.getOrString("last_event");
    const auto last_tool = entry.getOrString("last_tool");
    const auto buffer = entry.getOrString("buffer");
    const auto mode = entry.getOrString("mode");

    std::string event_label = last_event;
    if (!last_tool.empty()) event_label += ":" + last_tool;
    if (event_label.empty()) event_label = "—";

    const auto* mail_glyph = unread > 0 ? "📬" : "📭";
    const auto mail_color = unread > 0 ? kCyan : kDim;
    const bool has_input = !buffer.empty() && buffer != "(empty)";

    (void)process;  // axes are available for future per-axis columns
    (void)turn;

    std::println(
        "{}{:<14}{} {}  {}{:<10}{} {}{} {:>2}{}  {:<22} {:>7}  {:<6} "
        "{}{:<28}{}{}",
        agentColor(name), name, kReset, stateGlyph(st),
        stateColor(st), stateName(st), kReset, mail_color, mail_glyph,
        unread, kReset, event_label, formatAge(age_s),
        formatMode(mode), has_input ? kYellow : kDim,
        formatInput(buffer), kReset, kClearEol);
    ++rendered;
  }
  if (rendered == 0) {
    std::println("{}(no live agents){}{}", kDim, kReset, kClearEol);
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
