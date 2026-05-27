cmake_minimum_required(VERSION 3.25)
project(claude-bus CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd/LSP can resolve C++23 headers
# (std::print, std::expected, ...). A symlink at the repo root points
# at build/compile_commands.json; see .clangd for additional flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra -Wpedantic)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# Agent state derivation + pane helpers. Shared with the bus binary;
# kept as a static library only for compile-time isolation, no longer
# needed by separate executables (watcher / mailbox are retired).
add_library(bus_agent_status STATIC
    src/agent_status.cpp
    src/pane.cpp
)
target_include_directories(bus_agent_status PUBLIC src)

# The unified `bus` binary. Owns the broker daemon, all subcommand
# handlers, RPC, JSON, topic logs, and the delivery loop.
add_executable(bus
    src/bus_main.cpp
    src/bus.cpp
    src/broker.cpp
    src/delivery.cpp
    src/dispatch.cpp
    src/json_min.cpp
    src/rpc.cpp
    src/topic_log.cpp
    src/topic_registry.cpp
    src/sub/sub_agent_bar.cpp
    src/sub/sub_agents.cpp
    src/sub/sub_broker.cpp
    src/sub/sub_consume.cpp
    src/sub/sub_events.cpp
    src/sub/sub_inbox.cpp
    src/sub/sub_lifecycle.cpp
    src/sub/sub_meta.cpp
    src/sub/sub_monitor.cpp
    src/sub/sub_pane.cpp
    src/sub/sub_produce.cpp
    src/sub/sub_state.cpp
    src/sub/sub_topic.cpp
)
target_include_directories(bus PRIVATE src)
target_link_libraries(bus PRIVATE bus_agent_status)
