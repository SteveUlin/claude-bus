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

# Pure, dependency-light broker logic — no zellij, no sockets, no clock
# beyond what the caller passes. Linked into both the `bus` binary and
# the `bus_tests` unit suite so the tested code IS the shipped code.
# (tail_reader is header-only; computeAxes lives in bus_agent_status.)
add_library(bus_core STATIC
    src/event.cpp
    src/json_min.cpp
    src/recovery.cpp
    src/topic_log.cpp
)
target_include_directories(bus_core PUBLIC src)

# Agent state derivation + pane helpers. Shared with the bus binary;
# kept as a static library only for compile-time isolation, no longer
# needed by separate executables (watcher / mailbox are retired).
add_library(bus_agent_status STATIC
    src/agent_status.cpp
    src/pane.cpp
    src/task_model.cpp
)
target_include_directories(bus_agent_status PUBLIC src)
# readAgents parses each event line with json::parse (json_min), so the
# json symbols must resolve after this archive on the link line.
target_link_libraries(bus_agent_status PUBLIC bus_core)

# Build-commit stamp (D4 code-drift detection). Capture the source commit at
# configure time into a generated header; agent-launch compares it against
# landed main and auto-rebuilds bin/bus on drift. Colocated git HEAD == the
# jj commit_id, so this matches what agent-launch reads via `jj log -r main`.
# Re-runs every configure — the auto-rebuild does `cmake -S . -B build` first,
# so a deployed binary's stamp tracks the source it built from. A plain
# incremental `cmake --build` won't refresh it (no re-configure); that only
# affects local dev `bus version`, not the auto-rebuild path. "unknown" if
# git is unavailable.
execute_process(
    COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE BUS_BUILD_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT BUS_BUILD_COMMIT)
    set(BUS_BUILD_COMMIT "unknown")
endif()
configure_file(${CMAKE_SOURCE_DIR}/src/build_info.h.in
               ${CMAKE_BINARY_DIR}/build_info.h @ONLY)

# The unified `bus` binary. Owns the broker daemon, all subcommand
# handlers, RPC, JSON, topic logs, and the delivery loop.
add_executable(bus
    src/bus_main.cpp
    src/bus.cpp
    src/broker.cpp
    src/delivery.cpp
    src/dispatch.cpp
    src/rpc.cpp
    src/topic_registry.cpp
    src/trigger_feed.cpp
    src/sub/sub_agent_bar.cpp
    src/sub/sub_agents.cpp
    src/sub/sub_broker.cpp
    src/sub/sub_consume.cpp
    src/sub/sub_events.cpp
    src/sub/sub_inbox.cpp
    src/sub/sub_lifecycle.cpp
    src/sub/sub_log.cpp
    src/sub/sub_meta.cpp
    src/sub/sub_msg.cpp
    src/sub/sub_monitor.cpp
    src/sub/sub_pane.cpp
    src/sub/sub_produce.cpp
    src/sub/sub_roles.cpp
    src/sub/sub_state.cpp
    src/sub/sub_tasks.cpp
    src/sub/sub_topic.cpp
    src/sub/sub_triggers.cpp
    src/sub/sub_verify.cpp
)
target_include_directories(bus PRIVATE src ${CMAKE_BINARY_DIR})
target_link_libraries(bus PRIVATE bus_core bus_agent_status)

# Unit tests for the pure logic (json::parse, topic::parseFrom,
# computeAxes, the TailReader split/offset dance). Minimal in-tree
# harness, no third-party dependency. Binary lands in build/, not bin/.
# Run with `ctest --test-dir build` or `./build/bus_tests`.
enable_testing()
add_executable(bus_tests
    tests/unit/harness.cpp
    tests/unit/test_json.cpp
    tests/unit/test_event.cpp
    tests/unit/test_topic_log.cpp
    tests/unit/test_agent_status.cpp
    tests/unit/test_fold.cpp
    tests/unit/test_readiness.cpp
    tests/unit/test_doorbell_readiness.cpp
    tests/unit/test_tail_reader.cpp
    tests/unit/test_pane_cache.cpp
    tests/unit/test_retention.cpp
    tests/unit/test_topic_registry.cpp
    tests/unit/test_task_graph.cpp
    tests/unit/test_recovery.cpp
    src/topic_registry.cpp
)
target_include_directories(bus_tests PRIVATE src tests/unit)
target_link_libraries(bus_tests PRIVATE bus_core bus_agent_status)
set_target_properties(bus_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME unit COMMAND bus_tests)
