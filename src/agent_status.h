// claude-bus shared agent-status helpers.
//
// Both bin/monitor (table view) and bin/agent-bar (per-tab strip) derive
// the same view of an agent: latest hook event, computed lifecycle state,
// mailbox depth, pane-state introspection, attachment, prompt-bar color.
// Keep the derivation in one place so the two surfaces never disagree.
//
// All exits are best-effort: subprocess failures (zellij absent, agent
// gone, pane-state crash) surface as falsy fields rather than exceptions.

#pragma once

#include "pane.h"  // PaneState struct + paneState() live here now.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace bus {

// Latest event the agent emitted via the hooks pipeline. Empty `event`
// means we have never seen the agent in events.jsonl.
struct AgentEvent {
  std::string event;              // UserPromptSubmit / PreToolUse / Stop / ...
  std::string tool;               // payload.tool_name when PreToolUse
  std::string source;             // payload.source when SessionStart
                                  // ("compact" / "resume" / "startup")
  std::string notification_type;  // payload.notification_type when
                                  // Notification ("idle_prompt" means
                                  // claude is at the prompt waiting)
  std::int64_t ts_ms{};
};

struct AgentInfo {
  AgentEvent last;
};

// Lifecycle states. Names mirror what the monitor table prints; bar
// renderers map them onto compact glyphs.
enum class State {
  New,         // tracked but no event observed yet
  Starting,    // last event SessionStart, no turn yet
  Idle,        // last event Stop, no mail, pane exists
  HasMail,     // last event Stop, mail waiting, pane exists
  Working,     // last non-Stop event recent (<5 min)
  Stuck,       // last non-Stop event old (>5 min)
  Compacting,  // SessionStart source=compact — claude is summarizing
               //   the conversation; the agent looks idle from events
               //   but is actually busy. Auto-clears on the next event.
  NeedsInput,  // PreToolUse for AskUserQuestion — agent is blocked on
               //   the human picking an answer.
  BootStuck,   // SessionStart fired > 30s ago with no follow-up event.
               //   Pure hook-based detection — a healthy resume emits
               //   Notification(idle_prompt) within seconds; the absence
               //   is almost certainly the resume-summary modal hanging
               //   the boot.
  Ended,       // last event SessionEnd
  Gone,        // pane no longer exists
};

// Orthogonal axes. Each captures one independent dimension of agent
// status; surfaces choose which to render rather than collapsing every
// uncertainty into the single State enum. State below is now a derived
// view computed from these axes — keep it for callers not yet migrated.
//
//   Process — is the agent process alive? Derived from SessionStart /
//             SessionEnd / pane existence + the boot-completion signal
//             (any non-SessionStart event after SessionStart). Stuck
//             means "SessionStart > 30 s ago, no follow-up" — almost
//             certainly the resume-summary modal blocking the boot.
//   Turn    — what's the agent doing within its current life? Only
//             meaningful when Process == Alive. None otherwise.
//   Mail    — does the agent have unread inbox? Owned by the broker.
//   Tui     — can we write to the TUI right now? Read-only at write
//             time; never used to lift / lower lifecycle state.
enum class ProcessAxis {
  New,         // never observed in events.jsonl
  Starting,    // SessionStart seen, no follow-up yet (boot grace window)
  Compacting,  // SessionStart source=compact within 60 s — internal work
  Alive,       // boot complete (Notification or any post-start event)
  Stuck,       // SessionStart > 30 s ago, no follow-up — boot hang
  Ended,       // SessionEnd received
  Gone,        // pane vanished without SessionEnd
};

enum class TurnAxis {
  None,        // process not Alive — axis N/A
  Ready,       // last event Stop or Notification(idle_prompt)
  Working,     // last non-Stop event, age < 5 min
  Stuck,       // last non-Stop event, age > 5 min
  NeedsInput,  // AskUserQuestion or Notification(permission_prompt)
  Compacting,  // SessionStart source=compact within 60 s
};

enum class MailAxis {
  None,
  Pending,
};

enum class TuiAxis {
  Writable,  // mode == INSERT — safe to write
  Locked,    // mode == NORMAL / VISUAL / LOCKED — refuses writes
  Unknown,   // mode == unknown or dump failed — no opinion
};

struct AgentAxes {
  ProcessAxis process{ProcessAxis::New};
  TurnAxis turn{TurnAxis::None};
  MailAxis mail{MailAxis::None};
  TuiAxis tui{TuiAxis::Unknown};
};

auto computeAxes(const AgentInfo& a, std::size_t unread,
                 std::int64_t now_ms, bool pane_exists,
                 const PaneState* pane = nullptr) -> AgentAxes;

// Stable string names for the axis enums. Used by the broker's state
// RPC to put axes on the wire and by viewers to parse them back.
auto axisName(ProcessAxis p) -> std::string_view;
auto axisName(TurnAxis t) -> std::string_view;
auto axisName(MailAxis m) -> std::string_view;
auto axisName(TuiAxis u) -> std::string_view;

// Inverse — parse a string back to an axis enum. Returns the default
// (.New / .None / .Unknown) on miss; never throws.
auto processAxisFrom(std::string_view s) -> ProcessAxis;
auto turnAxisFrom(std::string_view s) -> TurnAxis;
auto mailAxisFrom(std::string_view s) -> MailAxis;
auto tuiAxisFrom(std::string_view s) -> TuiAxis;

auto stateName(State s) -> std::string_view;
auto stateColor(State s) -> std::string_view;

// One emoji per state. Cheap shape sulin can scan without reading the
// label text; bar, monitor, and any future surface use it so the
// visual idiom stays consistent. Each glyph occupies two display cells
// in ghostty (most are double-width emoji; the few text-presentation
// pictograms carry a trailing space to match).
auto stateGlyph(State s) -> std::string_view;

// Compatibility shim: derives a single State from computeAxes. New
// callers should prefer computeAxes — it preserves the per-dimension
// uncertainty that State has to collapse. pane is forwarded to
// computeAxes for the TuiAxis read; lifecycle state is hook-only and
// no longer depends on it.
auto computeState(const AgentInfo& a, std::size_t unread,
                  std::int64_t now_ms, bool pane_exists,
                  const PaneState* pane = nullptr) -> State;

// Read /tmp/claude-bus/events.jsonl, return latest event per agent.
// `filter` restricts to specific agents when non-empty.
auto readAgents(const std::string& log_path,
                const std::set<std::string>& filter)
    -> std::map<std::string, AgentInfo>;

// readPaneState is a compatibility alias for the in-process paneState()
// defined in pane.h. Older callers (monitor / agent-bar) kept the
// `read` prefix; new code should call paneState() directly.
inline auto readPaneState(const std::string& name) -> PaneState {
  return paneState(name);
}

// Per-agent color stored at ~/.cache/claude-bus/agents/NAME.color, one of
// claude's 8 /color choices. Returns an ANSI bright-color escape, or kBold
// when the file is missing or holds an unknown value.
auto agentColor(std::string_view name) -> std::string_view;

// Is any zellij client focused on this agent's pane? Pure observation —
// the bus deliberately does NOT change behavior based on focus. Sulin
// must be able to watch a pane (mail arriving, drains injecting, model
// responding) without halting the autonomous flow. Kept exposed for
// callers that genuinely want the signal; do not use it to gate mail.
auto isFocused(const std::string& name) -> bool;

// Does the agent have a recent presence file under <state>/presence/<name>?
// Written by the [bus-attach] sentinel (Ctrl+G a chord), removed by
// [bus-detach]. Expires after 1h so a forgotten attach can't mute the
// agent forever.
auto hasPresenceFile(const std::string& name) -> bool;

// Presence = explicit attach. The sentinel chord is the ONLY way to
// flip this; focus has no effect. Watcher and drain hook gate mail
// suppression on this exclusively.
auto isPresent(const std::string& name) -> bool;

auto nowMs() -> std::int64_t;

auto formatAge(std::int64_t age_s) -> std::string;
auto formatEvent(const AgentEvent& e) -> std::string;

namespace ansi {
constexpr std::string_view kReset = "\033[0m";
constexpr std::string_view kBold = "\033[1m";
constexpr std::string_view kDim = "\033[2m";
constexpr std::string_view kRed = "\033[31m";
constexpr std::string_view kYellow = "\033[33m";
constexpr std::string_view kCyan = "\033[36m";
constexpr std::string_view kGreen = "\033[32m";
constexpr std::string_view kBlue = "\033[34m";
constexpr std::string_view kMagenta = "\033[35m";
constexpr std::string_view kBrightRed = "\033[91m";
constexpr std::string_view kBrightGreen = "\033[92m";
constexpr std::string_view kBrightYellow = "\033[93m";
constexpr std::string_view kAltOn = "\033[?1049h";
constexpr std::string_view kAltOff = "\033[?1049l";
constexpr std::string_view kCursorHide = "\033[?25l";
constexpr std::string_view kCursorShow = "\033[?25h";
constexpr std::string_view kCursorHome = "\033[H";
constexpr std::string_view kClearEol = "\033[K";
constexpr std::string_view kClearBelow = "\033[J";
}  // namespace ansi

}  // namespace bus
