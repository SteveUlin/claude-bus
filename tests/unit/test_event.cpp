#include "event.h"
#include "harness.h"

#include <string>

using namespace bus;

// ---- the core correctness fix: top-level vs payload-nested keys ----

TEST(event_reads_toplevel_and_payload) {
  const std::string line =
      R"({"ts":"2026-05-30T20:58:39.468Z","session":"s","agent":"bast",)"
      R"("pane":"7","event":"PreToolUse","payload":{"tool_name":"Bash",)"
      R"("transcript_path":"/p/x.jsonl","cwd":"/w"}})";
  auto e = parseEvent(line);
  CHECK(e.has_value());
  CHECK_EQ(e->agent, std::string{"bast"});
  CHECK_EQ(e->event, std::string{"PreToolUse"});
  CHECK_EQ(e->session, std::string{"s"});
  CHECK_EQ(e->tool_name, std::string{"Bash"});          // from payload
  CHECK_EQ(e->transcript_path, std::string{"/p/x.jsonl"});
  CHECK_EQ(e->cwd, std::string{"/w"});
}

// The C3 / viewer misparse: a payload-nested "agent"/"event" must NOT
// shadow the record's own top-level values. A flat substring scan picked
// up whichever came first in the bytes — here the nested ones.
TEST(event_payload_nested_keys_do_not_shadow_toplevel) {
  const std::string line =
      R"({"ts":"2026-05-30T00:00:00.000Z","agent":"elodin","event":"PreToolUse",)"
      R"("payload":{"tool_name":"Bash","tool_input":)"
      R"({"command":"echo","agent":"victim","event":"FAKE"}}})";
  auto e = parseEvent(line);
  CHECK(e.has_value());
  CHECK_EQ(e->agent, std::string{"elodin"});   // NOT "victim"
  CHECK_EQ(e->event, std::string{"PreToolUse"}); // NOT "FAKE"
}

// ---- D3 cross-lane: bus-ack carries an inbox msg_id ----

TEST(event_busack_msg_id_from_payload) {
  const std::string line =
      R"({"ts":"2026-05-30T00:00:01.000Z","agent":"kvothe","event":"bus-ack",)"
      R"("payload":{"msg_id":"1780000000000-comms-abcd"}})";
  auto e = parseEvent(line);
  CHECK(e.has_value());
  CHECK_EQ(e->event, std::string{"bus-ack"});
  CHECK_EQ(e->msg_id, std::string{"1780000000000-comms-abcd"});
}

TEST(event_busack_msg_id_toplevel_fallback) {
  // If D3 stamps msg_id at the root instead of payload, the fallback
  // still surfaces it.
  const std::string line =
      R"({"ts":"2026-05-30T00:00:01.000Z","agent":"kvothe","event":"bus-ack",)"
      R"("msg_id":"1780000000001-comms-ef01","payload":{}})";
  auto e = parseEvent(line);
  CHECK(e.has_value());
  CHECK_EQ(e->msg_id, std::string{"1780000000001-comms-ef01"});
}

// ---- field coverage for the lifecycle viewers ----

TEST(event_sessionend_reason_from_payload) {
  // The exact shape sub_log / scanEvents care about: reason is nested.
  const std::string line =
      R"({"ts":"2026-05-30T00:00:02.000Z","agent":"a","event":"SessionEnd",)"
      R"("payload":{"reason":"clear"}})";
  auto e = parseEvent(line);
  CHECK(e.has_value());
  CHECK_EQ(e->reason, std::string{"clear"});
}

TEST(event_unescapes_payload_string) {
  // json_min decodes \" and \n inside a prompt body.
  const std::string line =
      R"({"agent":"a","event":"UserPromptSubmit","payload":)"
      R"({"prompt":"line one\nsay \"hi\""}})";
  auto e = parseEvent(line);
  CHECK(e.has_value());
  CHECK_EQ(e->prompt, std::string{"line one\nsay \"hi\""});
}

// ---- ts parsing + degenerate inputs ----

TEST(event_ts_ms_parsed) {
  auto e = parseEvent(
      R"({"ts":"1970-01-01T00:00:01.500Z","agent":"a","event":"Stop"})");
  CHECK(e.has_value());
  CHECK_EQ(e->ts_ms, std::int64_t{1500});
}

TEST(event_missing_payload_is_empty_not_crash) {
  auto e = parseEvent(R"({"agent":"a","event":"Stop"})");
  CHECK(e.has_value());
  CHECK_EQ(e->tool_name, std::string{});
  CHECK_EQ(e->reason, std::string{});
  CHECK_EQ(e->ts_ms, std::int64_t{0});  // no ts → 0
}

TEST(event_non_object_is_nullopt) {
  CHECK(!parseEvent("not json").has_value());
  CHECK(!parseEvent("[1,2,3]").has_value());
  CHECK(!parseEvent("").has_value());
}

TEST(event_empty_agent_still_parses) {
  // parseEvent doesn't filter; the caller decides. (readAgents skips
  // empty/unknown agents itself.)
  auto e = parseEvent(R"({"event":"Stop","payload":{}})");
  CHECK(e.has_value());
  CHECK_EQ(e->agent, std::string{});
}
