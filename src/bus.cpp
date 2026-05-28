#include "bus.h"

#include "sub.h"

#include <cstdio>
#include <print>

namespace bus {

// Subcommand registry. New subcommands append here in alphabetical-by-
// surface order so `bus help` lists them in a predictable layout.
// Keep this list short and obvious — over-categorization makes the
// `bus help` output a nuisance.
const Subcommand kSubcommands[] = {
    {"agent-bar", subAgentBar, "per-tab status strip for an agent"},
    {"agents", subAgents, "list registered agents (--kind/--session/--role, NAME, --json)"},
    {"broker", subBroker, "broker daemon: foreground; flock-singleton"},
    {"events", subEvents, "tail the bus event log"},
    {"help", subHelp, "list subcommands"},
    {"inflight", subInflight, "broker's in-flight deliveries (debug)"},
    {"introduce", subIntroduce, "composite card: registry + live state for an agent"},
    {"log", subLog, "colored lifecycle log (events.jsonl → started / working / finished / …)"},
    {"monitor", subMonitor, "dashboard of all bus agents"},
    {"msg", subMsg, "message operations: send, fetch, mail, slash, broadcast, etc."},
    {"pane-id", subPaneId, "resolve pane name → terminal_N"},
    {"pane-send", subPaneSend, "raw TTY write to a pane id (no flock)"},
    {"pane-state", subPaneState, "introspect a pane (mode, buffer, …)"},
    {"spawn", subSpawn, "open a new zellij tab for an agent"},
    {"state", subState, "broker's view of agent lifecycle"},
    {"topic", subTopic, "topic registry: create / list / show / inbox"},
    {"version", subVersion, "print version"},
};

const std::size_t kSubcommandsCount =
    sizeof(kSubcommands) / sizeof(kSubcommands[0]);

auto printUsage(std::FILE* out) -> void {
  std::println(out, "usage: bus <subcommand> [args...]");
  std::println(out, "");
  std::println(out, "subcommands:");
  for (std::size_t i = 0; i < kSubcommandsCount; ++i) {
    std::println(out, "  {:<14} {}", kSubcommands[i].name,
                 kSubcommands[i].summary);
  }
}

}  // namespace bus
