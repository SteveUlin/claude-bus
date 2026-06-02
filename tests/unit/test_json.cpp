#include "harness.h"
#include "json_min.h"
#include <string>

using namespace bus;

namespace {

// serialize∘parse identity at the string level (Value has no operator==).
auto roundtrips(std::string_view canonical) -> bool {
  auto v = json::parse(canonical);
  if (!v) return false;
  return json::serialize(*v) == canonical;
}

}  // namespace

TEST(json_roundtrip_scalars) {
  CHECK(roundtrips("null"));
  CHECK(roundtrips("true"));
  CHECK(roundtrips("false"));
  CHECK(roundtrips("0"));
  CHECK(roundtrips("42"));
  CHECK(roundtrips("-17"));
  CHECK(roundtrips("\"\""));
  CHECK(roundtrips("\"hello\""));
}

TEST(json_roundtrip_structures) {
  CHECK(roundtrips("[]"));
  CHECK(roundtrips("[1,2,3]"));
  CHECK(roundtrips("{}"));
  CHECK(roundtrips("{\"a\":1}"));
  // Object keys serialize in sorted (std::map) order — canonical form.
  CHECK(roundtrips("{\"a\":1,\"b\":\"two\",\"c\":[true,null]}"));
  CHECK(roundtrips("{\"outer\":{\"inner\":[1,2]}}"));
}

TEST(json_typed_field_reads) {
  auto v = json::parse(R"({"event":"Stop","count":3,"done":true})");
  CHECK(v.has_value());
  CHECK(v->isObject());
  CHECK_EQ(v->getOrString("event"), std::string{"Stop"});
  CHECK_EQ(v->getOrInt("count"), 3);
  CHECK_EQ(v->getOrBool("done"), true);
  // Absent keys fall back to the supplied default.
  CHECK_EQ(v->getOrString("missing", "def"), std::string{"def"});
  CHECK_EQ(v->getOrInt("missing", -1), -1);
}

// The shadowing hazard that motivates the readAgents → json_min migration
// (roadmap 1.4 / broker-internals §2.3): the brittle extractField does a
// flat substring scan and returns the FIRST "agent" match — which a nested
// payload.agent can shadow. A real parser reads the top-level key correctly.
TEST(json_nested_key_does_not_shadow_toplevel) {
  auto v = json::parse(
      R"({"agent":"bob","event":"PreToolUse","payload":{"agent":"NESTED","tool_name":"Bash"}})");
  CHECK(v.has_value());
  CHECK_EQ(v->getOrString("agent"), std::string{"bob"});  // not "NESTED"
  const auto* payload = v->get("payload");
  CHECK(payload != nullptr && payload->isObject());
  CHECK_EQ(payload->getOrString("tool_name"), std::string{"Bash"});
}

TEST(json_string_escapes_roundtrip) {
  CHECK(roundtrips(R"("a\"b")"));   // embedded quote
  CHECK(roundtrips(R"("a\\b")"));   // embedded backslash
}

TEST(json_rejects_malformed) {
  CHECK(!json::parse("{").has_value());
  CHECK(!json::parse("[1,2").has_value());
  CHECK(!json::parse("tru").has_value());
  CHECK(!json::parse("\"unterminated").has_value());
  CHECK(!json::parse("").has_value());
  // Trailing content after a complete document is an error (per the
  // parse() contract).
  CHECK(!json::parse("1 2").has_value());
}

// Documents the known float limitation (broker-internals §2.4 / 6.5.4):
// json_min is int-only, so a fractional value fails the WHOLE parse, not
// just the field. If this ever flips (float support added), update both
// this test and the doc.
TEST(json_floats_fail_whole_parse) {
  CHECK(!json::parse("1.5").has_value());
  CHECK(!json::parse(R"({"ratio":0.25})").has_value());
}

// Broker-hardening CRIT #1: unbounded recursion blows the stack → SIGSEGV.
// The depth cap (256) must turn pathological nesting into a clean parse
// error, never a crash. A balanced nest under the cap still parses.
TEST(json_rejects_deep_nesting_no_crash) {
  // ~100K open brackets: pre-fix this recurses ~100K deep and SIGSEGVs.
  // Post-fix it returns an error (the depth cap trips long before the
  // stack does).
  CHECK(!json::parse(std::string(100000, '[')).has_value());
  // A balanced array nested past the cap also errors.
  CHECK(!json::parse(std::string(300, '[') + std::string(300, ']'))
             .has_value());
  // A balanced nest comfortably under the cap parses fine.
  CHECK(json::parse(std::string(200, '[') + std::string(200, ']'))
            .has_value());
}
