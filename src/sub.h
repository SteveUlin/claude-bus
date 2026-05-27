#pragma once

// Declarations for every subcommand handler defined in src/sub/*.cpp.
// Each subcommand .cpp adds its prototype here; src/bus.cpp's registry
// references them.

#include <span>

namespace bus {

// Built-in meta subcommands (defined in src/sub/sub_meta.cpp).
auto subHelp(std::span<const char* const>) -> int;
auto subVersion(std::span<const char* const>) -> int;

// Pane helpers (defined in src/sub/sub_pane.cpp).
auto subPaneId(std::span<const char* const>) -> int;
auto subPaneState(std::span<const char* const>) -> int;
auto subSend(std::span<const char* const>) -> int;
auto subPaneSend(std::span<const char* const>) -> int;

// Events tail (defined in src/sub/sub_events.cpp).
auto subEvents(std::span<const char* const>) -> int;

// Viewers (defined in src/sub/sub_{monitor,inbox,agent_bar}.cpp).
auto subMonitor(std::span<const char* const>) -> int;
auto subInbox(std::span<const char* const>) -> int;
auto subAgentBar(std::span<const char* const>) -> int;

// Lifecycle (defined in src/sub/sub_lifecycle.cpp).
auto subSpawn(std::span<const char* const>) -> int;

// Broker daemon (defined in src/sub/sub_broker.cpp).
auto subBroker(std::span<const char* const>) -> int;

// Topic registry (defined in src/sub/sub_topic.cpp).
auto subTopic(std::span<const char* const>) -> int;

// Producers + consumers (defined in src/sub/sub_{produce,consume}.cpp).
auto subEnqueue(std::span<const char* const>) -> int;
auto subMail(std::span<const char* const>) -> int;
auto subMsg(std::span<const char* const>) -> int;
auto subSlash(std::span<const char* const>) -> int;
auto subBroadcast(std::span<const char* const>) -> int;
auto subFetch(std::span<const char* const>) -> int;
auto subPeek(std::span<const char* const>) -> int;
auto subBody(std::span<const char* const>) -> int;

// Broker introspection (defined in src/sub/sub_state.cpp).
auto subState(std::span<const char* const>) -> int;
auto subInflight(std::span<const char* const>) -> int;

// Agent registry (defined in src/sub/sub_agents.cpp).
auto subAgents(std::span<const char* const>) -> int;
auto subIntroduce(std::span<const char* const>) -> int;

}  // namespace bus
