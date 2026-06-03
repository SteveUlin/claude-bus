#include "agent_status.h"

#include "event.h"
#include "pane.h"
#include "state_paths.h"

#include <sys/stat.h>

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace bus {

namespace {

// Orchestration lease: an agent counts as Orchestrating for this long after
// its last Workflow dispatch, then auto-decays back to its real turn state.
// 90 s (auri's ratified ~60–120 s band) spans the inter-subagent gaps of a
// running workflow yet decays ~1–2 turns after it ends — short enough that a
// crashed/finished orchestrator never lies as Orchestrating for long.
constexpr std::int64_t kOrchestrationLeaseMs = 90 * 1000;

auto captureCommand(const std::string& cmd) -> std::string {
  FILE* pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) return {};
  std::string out;
  std::array<char, 512> buf{};
  while (std::fgets(buf.data(), buf.size(), pipe) != nullptr) {
    out.append(buf.data());
  }
  ::pclose(pipe);
  return out;
}

}  // namespace

auto axisName(ProcessAxis p) -> std::string_view {
  switch (p) {
    case ProcessAxis::New: return "new";
    case ProcessAxis::Starting: return "starting";
    case ProcessAxis::Compacting: return "compacting";
    case ProcessAxis::Alive: return "alive";
    case ProcessAxis::Stuck: return "stuck";
    case ProcessAxis::Ended: return "ended";
    case ProcessAxis::Gone: return "gone";
  }
  return "?";
}

auto axisName(TurnAxis t) -> std::string_view {
  switch (t) {
    case TurnAxis::None: return "none";
    case TurnAxis::Ready: return "ready";
    case TurnAxis::Working: return "working";
    case TurnAxis::Stuck: return "stuck";
    case TurnAxis::NeedsInput: return "needs_input";
    case TurnAxis::Compacting: return "compacting";
    case TurnAxis::Orchestrating: return "orchestrating";
  }
  return "?";
}

auto axisName(MailAxis m) -> std::string_view {
  switch (m) {
    case MailAxis::None: return "none";
    case MailAxis::Pending: return "pending";
  }
  return "?";
}

auto axisName(TuiAxis u) -> std::string_view {
  switch (u) {
    case TuiAxis::Writable: return "writable";
    case TuiAxis::Locked: return "locked";
    case TuiAxis::Unknown: return "unknown";
  }
  return "?";
}

auto processAxisFrom(std::string_view s) -> ProcessAxis {
  if (s == "starting") return ProcessAxis::Starting;
  if (s == "compacting") return ProcessAxis::Compacting;
  if (s == "alive") return ProcessAxis::Alive;
  if (s == "stuck") return ProcessAxis::Stuck;
  if (s == "ended") return ProcessAxis::Ended;
  if (s == "gone") return ProcessAxis::Gone;
  return ProcessAxis::New;
}

auto turnAxisFrom(std::string_view s) -> TurnAxis {
  if (s == "ready") return TurnAxis::Ready;
  if (s == "working") return TurnAxis::Working;
  if (s == "stuck") return TurnAxis::Stuck;
  if (s == "needs_input") return TurnAxis::NeedsInput;
  if (s == "compacting") return TurnAxis::Compacting;
  if (s == "orchestrating") return TurnAxis::Orchestrating;
  return TurnAxis::None;
}

auto mailAxisFrom(std::string_view s) -> MailAxis {
  return s == "pending" ? MailAxis::Pending : MailAxis::None;
}

auto tuiAxisFrom(std::string_view s) -> TuiAxis {
  if (s == "writable") return TuiAxis::Writable;
  if (s == "locked") return TuiAxis::Locked;
  return TuiAxis::Unknown;
}

auto stateName(State s) -> std::string_view {
  switch (s) {
    case State::New:
      return "NEW";
    case State::Starting:
      return "STARTING";
    case State::Idle:
      return "IDLE";
    case State::HasMail:
      return "HAS_MAIL";
    case State::Working:
      return "WORKING";
    case State::Orchestrating:
      return "ORCHESTRATING";
    case State::Stuck:
      return "STUCK";
    case State::Compacting:
      return "COMPACTING";
    case State::NeedsInput:
      return "NEEDS_INPUT";
    case State::BootStuck:
      return "BOOT_STUCK";
    case State::Ended:
      return "ENDED";
    case State::Gone:
      return "GONE";
  }
  return "?";
}

auto stateGlyph(State s) -> std::string_view {
  switch (s) {
    case State::New:
      return "🌱";
    case State::Starting:
      return "🚀";
    case State::Idle:
      return "💤";
    case State::HasMail:
      return "🔔";
    case State::Working:
      return "🔨";  // Single-codepoint emoji, unambiguous 2-cell width.
    case State::Orchestrating:
      return "🪐";  // Ringed planet: subagents orbit the orchestrator.
                    //   Single-codepoint, unambiguous 2-cell width.
    case State::Stuck:
      return "🚧";
    case State::Compacting:
      return "🌀";  // Single-codepoint emoji, unambiguous 2-cell width.
    case State::NeedsInput:
      return "🙋";
    case State::BootStuck:
      return "🪑";
    case State::Ended:
      return "🏁";
    case State::Gone:
      return "👻";
  }
  return "?";
}

auto stateColor(State s) -> std::string_view {
  switch (s) {
    case State::New:
      return ansi::kBlue;
    case State::Starting:
      return ansi::kBlue;
    case State::Idle:
      return ansi::kGreen;
    case State::HasMail:
      return ansi::kCyan;
    case State::Working:
      return ansi::kYellow;
    case State::Orchestrating:
      // Bright-green = "busy but RECEPTIVE" — the healthy, mail-takeable
      // sibling of Working's plain yellow. Distinct from Idle's plain green.
      return ansi::kBrightGreen;
    case State::Stuck:
      return ansi::kRed;
    case State::Compacting:
      // Magenta = "claude is doing internal work that looks idle but
      // isn't." Distinct from Working so the human can pick it out.
      return ansi::kMagenta;
    case State::NeedsInput:
      // Bright-yellow grabs attention without alarm-red, matches the
      // agent-bar intervention badge color.
      return ansi::kBrightYellow;
    case State::BootStuck:
      // Bright-red because the agent is silently un-recoverable until
      // the human attaches and dismisses the modal.
      return ansi::kBrightRed;
    case State::Ended:
      return ansi::kDim;
    case State::Gone:
      return ansi::kDim;
  }
  return ansi::kReset;
}

auto foldTurnState(AgentInfo& info) -> void {
  const auto& ev = info.last.event;
  const auto ts = info.last.ts_ms;
  if (ev == "UserPromptSubmit") {
    info.turn_start_ms = ts;
    info.open_tool.clear();
    info.open_tool_since_ms = 0;
  } else if (ev == "PreToolUse") {
    info.open_tool = info.last.tool;
    info.open_tool_since_ms = ts;
    // A Workflow dispatch puts the agent in an orchestration posture: it
    // fans subagents out in the background and returns immediately, so the
    // posture outlives the tool call. Anchor the lease here; computeAxes
    // TTL-decays it. (Workflow is the unambiguous events-only signal — a
    // background Task/Bash isn't distinguishable from a blocking one
    // without tool-input capture; that's the explicit-sentinel v2.)
    if (info.last.tool == "Workflow") info.last_orchestration_ms = ts;
  } else if (ev == "PostToolUse") {
    info.open_tool.clear();
    info.open_tool_since_ms = 0;
  } else if (ev == "Stop") {
    info.turn_start_ms = 0;
    info.open_tool.clear();
    info.open_tool_since_ms = 0;
  } else if (ev == "Notification") {
    if (info.last.notification_type == "idle_prompt") info.turn_start_ms = 0;
  } else if (ev == "SessionStart" || ev == "SessionEnd") {
    // New / ended session — reset the turn AND the orchestration lease so a
    // prior session's Workflow can't bleed into a fresh one.
    info.turn_start_ms = 0;
    info.open_tool.clear();
    info.open_tool_since_ms = 0;
    info.last_orchestration_ms = 0;
  }
}

auto computeAxes(const AgentInfo& a, std::size_t unread,
                 std::int64_t now_ms, bool pane_exists,
                 const PaneState* pane) -> AgentAxes {
  AgentAxes ax;
  const auto& ev = a.last.event;
  const auto age_s = a.last.ts_ms > 0 ? (now_ms - a.last.ts_ms) / 1000 : 0;

  // ---- Process axis ----
  if (ev.empty()) {
    ax.process = pane_exists ? ProcessAxis::Starting : ProcessAxis::New;
  } else if (!pane_exists) {
    ax.process =
        ev == "SessionEnd" ? ProcessAxis::Ended : ProcessAxis::Gone;
  } else if (ev == "SessionEnd") {
    ax.process = ProcessAxis::Ended;
  } else if (ev == "SessionStart") {
    // The "no Notification follow-up = stuck" heuristic only holds for
    // source=="startup". Other sources land at an already-active prompt
    // (resume, clear) or run an unbounded internal task (compact); none
    // of them are boot, so none can be boot-stuck.
    if (a.last.source == "compact") {
      // /compact summarises context — can take minutes on big windows.
      // Stays Compacting until a follow-up event arrives.
      ax.process = ProcessAxis::Compacting;
    } else if (a.last.source == "resume" || a.last.source == "clear") {
      // Resume inherits the prior prompt; /clear lands at a fresh
      // prompt. Neither fires Notification(idle_prompt) reliably, so
      // absence of a follow-up event is normal, not a stuck signal.
      ax.process = ProcessAxis::Alive;
    } else if (age_s > 30) {
      ax.process = ProcessAxis::Stuck;  // source=startup, boot hung
    } else {
      ax.process = ProcessAxis::Starting;
    }
  } else {
    // Any non-SessionStart, non-SessionEnd event after SessionStart means
    // claude is past boot and engaged in a turn or sitting at the prompt.
    ax.process = ProcessAxis::Alive;
  }

  // ---- Turn axis (only meaningful when process is Alive) ----
  if (ax.process != ProcessAxis::Alive) {
    ax.turn = TurnAxis::None;
  } else if (ev == "PreToolUse" && a.last.tool == "AskUserQuestion") {
    ax.turn = TurnAxis::NeedsInput;
  } else if (ev == "Notification") {
    ax.turn = a.last.notification_type == "permission_prompt"
                  ? TurnAxis::NeedsInput
                  : TurnAxis::Ready;
  } else if (ev == "Stop") {
    ax.turn = TurnAxis::Ready;
  } else if (ev == "SessionStart") {
    // Reaches here on source=="resume" or source=="clear" (other
    // SessionStart paths short-circuit via Process::Starting /
    // Compacting / Stuck, which set turn=None above). Both land at a
    // prompt waiting for input.
    ax.turn = TurnAxis::Ready;
  } else if (age_s > 5 * 60) {
    // PreToolUse / PostToolUse / UserPromptSubmit, but stale > 5 min — a
    // genuine stall outranks an orchestration lease (which is shorter than
    // this anyway, so it has already decayed).
    ax.turn = TurnAxis::Stuck;
  } else if (a.last_orchestration_ms > 0 &&
             now_ms - a.last_orchestration_ms < kOrchestrationLeaseMs) {
    // Mid-turn AND a Workflow dispatched within the lease window — the
    // receptive sibling of Working. Overrides ONLY Working (Ready / Stuck /
    // NeedsInput above are untouched), so the render gains a truthful label
    // with zero delivery change until the gate (wakeReadyForMail) honors it.
    ax.turn = TurnAxis::Orchestrating;
  } else {
    // PreToolUse / PostToolUse / UserPromptSubmit — mid-turn work.
    ax.turn = TurnAxis::Working;
  }

  // ---- Mail axis ----
  ax.mail = unread > 0 ? MailAxis::Pending : MailAxis::None;

  // ---- TUI axis (write-time only — never gates lifecycle state) ----
  if (pane == nullptr || !pane->ok) {
    ax.tui = TuiAxis::Unknown;
  } else if (pane->mode == "INSERT") {
    ax.tui = TuiAxis::Writable;
  } else if (pane->mode == "unknown") {
    ax.tui = TuiAxis::Unknown;
  } else {
    ax.tui = TuiAxis::Locked;
  }

  return ax;
}

auto computeState(const AgentInfo& a, std::size_t unread,
                  std::int64_t now_ms, bool pane_exists,
                  const PaneState* pane) -> State {
  // Compatibility shim — derive the single-state view from the
  // orthogonal axes. New callers should prefer computeAxes.
  const auto ax = computeAxes(a, unread, now_ms, pane_exists, pane);
  switch (ax.process) {
    case ProcessAxis::New:
      return State::New;
    case ProcessAxis::Gone:
      return State::Gone;
    case ProcessAxis::Ended:
      return State::Ended;
    case ProcessAxis::Stuck:
      return State::BootStuck;
    case ProcessAxis::Starting:
      return State::Starting;
    case ProcessAxis::Compacting:
      return State::Compacting;
    case ProcessAxis::Alive:
      break;
  }
  switch (ax.turn) {
    case TurnAxis::Ready:
      return ax.mail == MailAxis::Pending ? State::HasMail : State::Idle;
    case TurnAxis::Working:
      return State::Working;
    case TurnAxis::Orchestrating:
      return State::Orchestrating;
    case TurnAxis::Stuck:
      return State::Stuck;
    case TurnAxis::NeedsInput:
      return State::NeedsInput;
    case TurnAxis::Compacting:
      return State::Compacting;
    case TurnAxis::None:
      return State::Starting;  // shouldn't happen when process=Alive
  }
  return State::Starting;
}

auto wakeReadyForMail(const AgentAxes& ax, const PaneState* pane) -> bool {
  // Normal idle at the prompt.
  if (ax.process == ProcessAxis::Alive && ax.turn == TurnAxis::Ready) {
    return true;
  }
  // Post-compaction idle — SessionStart(source=compact) is the last event
  // and no follow-up comes; the agent is idle at the prompt.
  if (ax.process == ProcessAxis::Compacting) return true;
  // Boot ambiguity: a fresh spawn / >30s-idle boot reads Starting/Stuck from
  // events alone (indistinguishable from a wedged boot). An editable INSERT
  // prompt is the ground truth that claude is ready for its first input; a
  // wedged boot shows a modal (non-INSERT) and stays excluded — BOOT_STUCK
  // preserved.
  if ((ax.process == ProcessAxis::Starting ||
       ax.process == ProcessAxis::Stuck) &&
      pane != nullptr && pane->ok && pane->mode == "INSERT") {
    return true;
  }
  return false;
}

auto readAgents(const std::string& log_path,
                const std::set<std::string>& filter)
    -> std::map<std::string, AgentInfo> {
  std::map<std::string, AgentInfo> out;
  std::ifstream in{log_path};
  if (!in) return out;

  // One typed parse per line (parseEvent reads top-level agent/event/ts
  // from the document root and the per-turn fields from `payload`, so a
  // payload-nested `"agent"`/`"event"` can no longer be misattributed —
  // subsumes C3). The latest event per agent wins.
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto ev = parseEvent(line);
    if (!ev) continue;
    if (ev->agent.empty() || ev->agent == "unknown") continue;
    if (!filter.empty() && !filter.contains(ev->agent)) continue;

    auto& info = out[ev->agent];
    info.last.event = std::move(ev->event);
    info.last.tool = std::move(ev->tool_name);
    info.last.source = std::move(ev->source);
    info.last.notification_type = std::move(ev->notification_type);
    info.last.transcript_path = std::move(ev->transcript_path);
    info.last.ts_ms = ev->ts_ms;
    // D8 fold: carry turn/tool structure across the sequence (Part A).
    foldTurnState(info);
  }
  return out;
}

// readPaneState's body lives as an inline in agent_status.h now —
// just delegates to pane.h's paneState(). The popen-based version is
// gone with bin/pane-state.

auto agentColor(std::string_view name) -> std::string_view {
  const char* home = std::getenv("HOME");
  if (home == nullptr) return ansi::kBold;
  const auto path =
      std::format("{}/.cache/claude-bus/agents/{}.color", home, name);
  std::ifstream in{path};
  if (!in) return ansi::kBold;
  std::string color;
  std::getline(in, color);
  while (!color.empty() &&
         std::isspace(static_cast<unsigned char>(color.back()))) {
    color.pop_back();
  }
  if (color == "red") return "\033[91m";
  if (color == "green") return "\033[92m";
  if (color == "yellow") return "\033[93m";
  if (color == "blue") return "\033[94m";
  if (color == "purple") return "\033[95m";
  if (color == "cyan") return "\033[96m";
  if (color == "orange") return "\033[38;5;208m";
  if (color == "pink") return "\033[38;5;205m";
  return ansi::kBold;
}

namespace {

auto presencePath(const std::string& name) -> std::filesystem::path {
  return std::filesystem::path{stateRoot()} / "presence" / name;
}

}  // namespace

auto hasPresenceFile(const std::string& name) -> bool {
  struct stat st;
  const auto path = presencePath(name);
  if (::stat(path.c_str(), &st) != 0) return false;
  // Expire stale files so a forgotten attach can't mute an agent forever.
  using namespace std::chrono;
  const auto now =
      duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  return (now - static_cast<std::int64_t>(st.st_mtime)) < 3600;
}

auto isFocused(const std::string& name) -> bool {
  // Resolve agent name -> terminal_N in-process via pane.h.
  const auto pane = paneId(name);
  if (pane.empty()) return false;

  // Compare against the second column of every list-clients row.
  const auto clients =
      captureCommand("zellij action list-clients 2>/dev/null");
  if (clients.empty()) return false;
  std::istringstream in{clients};
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (first) {
      first = false;
      continue;
    }
    std::istringstream ls{line};
    std::string client, pid;
    if (ls >> client >> pid && pid == pane) return true;
  }
  return false;
}

auto isPresent(const std::string& name) -> bool {
  return hasPresenceFile(name);
}

auto nowMs() -> std::int64_t {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

auto formatAge(std::int64_t age_s) -> std::string {
  if (age_s < 0) return "?";
  if (age_s < 60) return std::format("{}s", age_s);
  if (age_s < 3600) return std::format("{}m{:02}s", age_s / 60, age_s % 60);
  return std::format("{}h{:02}m", age_s / 3600, (age_s % 3600) / 60);
}

auto formatEvent(const AgentEvent& e) -> std::string {
  if (e.event.empty()) return "—";
  if (e.tool.empty()) return e.event;
  return std::format("{}:{}", e.event, e.tool);
}

}  // namespace bus
