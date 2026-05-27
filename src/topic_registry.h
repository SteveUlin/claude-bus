#pragma once

// Topic registry — declarative table of every topic the broker knows
// about, persisted to $STATE/topics.json. Each topic declares its
// `kind`, which decides how the broker delivers records on it
// (phase 4b.3 wires the kind → delivery routing).
//
// Persistence is crash-safe: writes go to topics.json.tmp then rename.

#include "json_min.h"
#include "types.h"

#include <expected>
#include <map>
#include <string>
#include <variant>
#include <vector>
#include <string_view>
#include <vector>

namespace bus {

struct AgentInboxConfig {
  std::string agent;
};

struct TuiCommandsConfig {
  std::string agent;
};

struct PubsubConfig {};

struct WorkQueueConfig {};

struct BlackboardConfig {};

struct AppendLogConfig {};

using TopicKindConfig = std::variant<
    std::monostate,
    AgentInboxConfig,
    TuiCommandsConfig,
    PubsubConfig,
    WorkQueueConfig,
    BlackboardConfig,
    AppendLogConfig>;

// A topic is a typed, persisted message stream.
struct TopicConfig {
  std::string name;
  std::string kind;
  std::int64_t max_record_bytes{4096};
  std::int64_t retention_ms{0};  // 0 means unbounded
  json::Value kind_config{json::Value::null_()};
  TopicKindConfig parsed_config;  // free-form per-kind blob

  // Serialize / deserialize for topics.json.
  auto toJson() const -> json::Value;
  static auto fromJson(const json::Value& v)
      -> Result<TopicConfig>;
};

// Known kinds. Other strings are accepted by the registry but rejected
// at delivery time in 4b.3.
constexpr std::string_view kKindAgentInbox = "agent-inbox";
constexpr std::string_view kKindTuiCommands = "tui-commands";
constexpr std::string_view kKindWorkQueue = "work-queue";
constexpr std::string_view kKindPubsub = "pubsub";
constexpr std::string_view kKindBlackboard = "blackboard";
constexpr std::string_view kKindAppendLog = "append-log";

class TopicRegistry {
 public:
  explicit TopicRegistry(std::string path);

  // Read topics.json into memory. ENOENT is success (empty registry).
  auto load() -> Result<void>;

  // Write the in-memory state atomically (tmp + rename). Called by
  // create() etc.; callers don't usually invoke directly.
  auto save() const -> Result<void>;

  // Declare a new topic. Errors if name is invalid or already exists.
  // Persists to disk on success.
  auto create(TopicConfig cfg) -> Result<void>;

  // Existence check.
  auto contains(std::string_view name) const -> bool;

  // Read-only access. Returns nullptr on miss.
  auto get(std::string_view name) const -> const TopicConfig*;

  // Snapshot of all topics.
  auto list() const -> std::vector<TopicConfig>;

  // Auto-create from name pattern: inbox-<X> → agent-inbox,
  // commands-<X> → tui-commands. Returns the resulting config (whether
  // pre-existing or freshly created). On unknown patterns, returns
  // an error.
  auto getOrAutoCreate(std::string_view name)
      -> Result<TopicConfig>;

 private:
  std::string path_;
  std::map<std::string, TopicConfig, std::less<>> topics_;
};

// Validate topic name characters. Lowercase + digits + hyphen.
auto isValidTopicName(std::string_view name) -> bool;

}  // namespace bus
