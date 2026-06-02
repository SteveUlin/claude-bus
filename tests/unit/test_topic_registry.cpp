#include "harness.h"
#include "topic_registry.h"

#include <string>

using namespace bus;

namespace {

auto tmpPath() -> std::string {
  static int n = 0;
  return std::string{"/tmp/claude-bus-test-registry-"} + std::to_string(++n) +
         ".json";
}

}  // namespace

// A bare agent name auto-creates the inbox with the agent derived from the
// suffix — the normal, intended path.
TEST(getOrAutoCreate_inbox_normal) {
  TopicRegistry reg{tmpPath()};
  auto r = reg.getOrAutoCreate("inbox-human");
  CHECK(r.has_value());
  if (r) {
    CHECK_EQ(r->kind, std::string{kKindAgentInbox});
    CHECK_EQ(r->kind_config.getOrString("agent"), std::string{"human"});
  }
}

TEST(getOrAutoCreate_commands_normal) {
  TopicRegistry reg{tmpPath()};
  auto r = reg.getOrAutoCreate("commands-bob");
  CHECK(r.has_value());
  if (r) {
    CHECK_EQ(r->kind, std::string{kKindTuiCommands});
    CHECK_EQ(r->kind_config.getOrString("agent"), std::string{"bob"});
  }
}

// The double-prefix footgun: passing a topic name where an agent name is
// expected (e.g. `bus msg mail inbox-human` → "inbox-" + "inbox-human")
// must be REFUSED, not silently materialized as inbox-inbox-human.
TEST(getOrAutoCreate_rejects_nested_inbox_prefix) {
  TopicRegistry reg{tmpPath()};
  auto r = reg.getOrAutoCreate("inbox-inbox-human");
  CHECK(!r.has_value());
  CHECK(!reg.contains("inbox-inbox-human"));
}

TEST(getOrAutoCreate_rejects_nested_commands_prefix) {
  TopicRegistry reg{tmpPath()};
  auto r = reg.getOrAutoCreate("inbox-commands-bob");
  CHECK(!r.has_value());
  CHECK(!reg.contains("inbox-commands-bob"));
}

TEST(getOrAutoCreate_rejects_nested_commands_topic) {
  TopicRegistry reg{tmpPath()};
  auto r = reg.getOrAutoCreate("commands-commands-bob");
  CHECK(!r.has_value());
  CHECK(!reg.contains("commands-commands-bob"));
}
