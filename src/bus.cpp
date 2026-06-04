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
    {"done", subDone, "stamp a durable completion claim: done \"<task>\" \"<artifact>\""},
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
    {"recover", subRecover,
     "relaunch a wedged agent: kill → respawn-loop → verify (exit 0/10/20/30)"},
    {"roles", subRoles, "list role files (no args) or print one role's body (NAME)"},
    {"spawn", subSpawn, "open a new zellij tab for an agent"},
    {"restore-peers", subRestorePeers,
     "re-spawn persisted dynamic peers (run at fleet startup)"},
    {"despawn", subDespawn,
     "tear down a dynamic peer: prune its registry entry + close its tab"},
    {"state", subState, "broker's view of agent lifecycle"},
    {"state-root", subStateRoot, "print the resolved bus state dir (state_paths.h)"},
    {"task", subTask, "set an agent's monitor TASK column: task set AGENT \"<action>\""},
    {"tasks", subTasks, "fleet task model: per-task owner/state/done + owner-liveness overlay"},
    {"topic", subTopic, "topic registry: create / list / show / inbox"},
    {"triggers", subTriggers,
     "P3 context-mgmt trigger feed: per-agent urgency × safety → ACT/WAIT/OK"},
    {"verify", subVerify, "claimed-done vs artifact-present: flag false done-claims"},
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
