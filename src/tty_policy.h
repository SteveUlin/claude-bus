#pragma once

// Off-TTY is the FLEET DEFAULT: an agent's mail is delivered by its own
// drain hook (as additionalContext), not by the broker typing into the
// pane. The ONLY agents kept on the TTY push path are the durable
// opt-out set — sourced from COMMITTED config, never from ephemeral
// $STATE (a $STATE/off-tty sentinel is exactly what kept getting wiped
// on relaunch, silently reverting the fleet to TTY).
//
// Opt-out source of truth:
//   * "comms" — compiled in, so it can NEVER be lost. comms is the
//     human-facing pane sulin attaches to; the doorbell's presence gate
//     won't wake an attached pane, so off-TTY would delay agent->comms
//     mail whenever sulin is attached-but-idle. TTY push is immediate.
//   * $CLAUDE_BUS_TTY_AGENTS — a comma/space-separated list (set durably
//     in layouts/fleet.kdl's broker pane) for any additional opt-outs,
//     without a rebuild.

#include <array>
#include <cstdlib>
#include <string_view>

namespace bus {

// Roles excluded from automatic context clearing (comms / primary maintain
// high cross-thread continuity and should only clear on explicit human cue —
// see clear-policy.md §6). Used by delivery.cpp (maybeAutoClear) and
// recovery_actor.cpp (R1 idle-context triage).
inline constexpr std::array<std::string_view, 2> kNoClearRoles{"comms",
                                                                "primary"};

// True iff `agent` is on the TTY push path (i.e. in the opt-out set).
inline auto isTtyAgent(std::string_view agent) -> bool {
  if (agent == "comms") return true;  // bedrock opt-out — see header
  const char* env = std::getenv("CLAUDE_BUS_TTY_AGENTS");
  if (env == nullptr || *env == '\0') return false;
  const std::string_view list{env};
  std::size_t i = 0;
  while (i < list.size()) {
    while (i < list.size() && (list[i] == ',' || list[i] == ' ')) ++i;
    std::size_t j = i;
    while (j < list.size() && list[j] != ',' && list[j] != ' ') ++j;
    if (j > i && list.substr(i, j - i) == agent) return true;
    i = j;
  }
  return false;
}

// True iff `agent` uses off-TTY (drain-hook) delivery — the default.
inline auto isOffTty(std::string_view agent) -> bool {
  return !isTtyAgent(agent);
}

}  // namespace bus
