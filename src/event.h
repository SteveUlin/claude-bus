#pragma once

// One typed events.jsonl record, parsed ONCE (roadmap D7). Every consumer
// — agent_status::readAgents, delivery::scanEvents, and the sub_events /
// sub_log viewers — used to hand-roll its own `extractStr`
// substring scanner with subtly divergent escape handling, and several did
// a FLAT search that mismatched a payload-nested `"agent"`/`"event"` for
// the record's own top-level key (the C3 misparse, also latent in the
// viewers). parseEvent replaces all of them with one json_min parse that
// respects the document shape.
//
// Wire shape (log-event.sh): the document root carries ts/session/agent/
// pane/event; the hook's stdin payload is nested under `payload`. The
// per-turn fields (tool_name, source, notification_type, transcript_path,
// reason, prompt, last_assistant_message, cwd, message) therefore live
// inside `payload`, NOT at the root.
//
// D3 cross-lane (kvothe): the drain hook emits a `{event:"bus-ack",...}`
// record carrying an inbox msg_id. `msg_id` is read from payload.msg_id
// with a top-level fallback, so D3 rebases onto this parser regardless of
// which level it lands the field on.

#include "json_min.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bus {

struct Event {
  // Document root.
  std::string ts;       // "2026-05-30T20:58:39.468Z"
  std::string session;
  std::string agent;
  std::string pane;
  std::string event;    // hook name (PreToolUse/Stop/SessionStart/…) or
                        // a synthetic type like "bus-ack".

  // payload.* — empty string when the key is absent for this event kind.
  std::string tool_name;              // PreToolUse / PostToolUse
  std::string source;                 // SessionStart: startup|resume|compact
  std::string notification_type;      // Notification: idle_prompt|permission_prompt
  std::string transcript_path;        // every event — names the live session
  std::string reason;                 // SessionEnd: clear|compact|exit|…
  std::string prompt;                 // UserPromptSubmit
  std::string last_assistant_message; // Stop
  std::string cwd;
  std::string message;
  std::string msg_id;                 // D3 bus-ack (payload.msg_id, else top-level)

  std::int64_t ts_ms{0};              // ts parsed to ms-since-epoch (UTC); 0 on fail

  // Escape hatch: the parsed payload object (null when absent), for the
  // rare field no struct member covers. Keeps the common path typed
  // without forcing a member per key.
  json::Value payload{json::Value::null_()};
};

// Parse one events.jsonl line. nullopt when the line isn't a JSON object.
// A record with no `agent` still parses (the caller decides whether to
// skip it); ts_ms is derived from ts.
auto parseEvent(std::string_view line) -> std::optional<Event>;

// Parse "YYYY-MM-DDTHH:MM:SS.mmmZ" to ms since epoch (UTC). 0 on failure.
// Shared so every surface dates an event identically.
auto parseIso8601Ms(std::string_view s) -> std::int64_t;

}  // namespace bus
