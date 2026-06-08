#include "topic_registry.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <print>
#include <sstream>
#include <string>
#include <utility>

namespace bus {

namespace {

auto isValidTopicName(std::string_view name) -> bool {
  if (name.empty() || name.size() > 128) return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

}  // namespace

auto topicKindFromStr(std::string_view s) -> TopicKind {
  if (s == kKindAgentInbox) return TopicKind::AgentInbox;
  if (s == kKindTuiCommands) return TopicKind::TuiCommands;
  if (s == kKindWorkQueue)   return TopicKind::WorkQueue;
  if (s == kKindPubsub)      return TopicKind::Pubsub;
  if (s == kKindBlackboard)  return TopicKind::Blackboard;
  if (s == kKindAppendLog)   return TopicKind::AppendLog;
  return TopicKind::Unknown;
}

auto topicKindToStr(TopicKind k) -> std::string_view {
  switch (k) {
    case TopicKind::AgentInbox: return kKindAgentInbox;
    case TopicKind::TuiCommands: return kKindTuiCommands;
    case TopicKind::WorkQueue:   return kKindWorkQueue;
    case TopicKind::Pubsub:      return kKindPubsub;
    case TopicKind::Blackboard:  return kKindBlackboard;
    case TopicKind::AppendLog:   return kKindAppendLog;
    case TopicKind::Unknown:     return "";
  }
  return "";  // unreachable, silences -Wreturn-type
}

// Parse a kind_config JSON object into a TopicKindConfig based on TopicKind.
// kc may be nullptr (no kind_config present in JSON).
auto kindConfigFromJson(TopicKind kind, const json::Value* kc)
    -> TopicKindConfig {
  switch (kind) {
    case TopicKind::AgentInbox:
      return AgentInboxConfig{kc ? kc->getOrString("agent") : ""};
    case TopicKind::TuiCommands:
      return TuiCommandsConfig{kc ? kc->getOrString("agent") : ""};
    case TopicKind::Pubsub: {
      std::vector<std::string> subs;
      if (kc != nullptr) {
        if (const auto* arr = kc->get("subscribers");
            arr != nullptr && arr->isArray()) {
          for (const auto& s : arr->asArray()) {
            if (s.isString()) subs.push_back(s.asString());
          }
        }
      }
      return PubsubConfig{std::move(subs)};
    }
    case TopicKind::WorkQueue:
      return WorkQueueConfig{};
    case TopicKind::Blackboard:
      return BlackboardConfig{};
    case TopicKind::AppendLog:
      return AppendLogConfig{};
    case TopicKind::Unknown:
      return std::monostate{};
  }
  return std::monostate{};
}

namespace {

// Build the kind_config JSON object from parsed_config (for serialization).
auto kindConfigToJson(const TopicKindConfig& pc) -> std::optional<json::Value> {
  return std::visit(
      [](const auto& v) -> std::optional<json::Value> {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, AgentInboxConfig> ||
                      std::is_same_v<T, TuiCommandsConfig>) {
          std::map<std::string, json::Value> kc;
          kc.insert({"agent", json::Value::from(v.agent)});
          return json::Value::fromObject(std::move(kc));
        } else if constexpr (std::is_same_v<T, PubsubConfig>) {
          std::vector<json::Value> subs;
          for (const auto& s : v.subscribers)
            subs.push_back(json::Value::from(s));
          std::map<std::string, json::Value> kc;
          kc.insert({"subscribers", json::Value::fromArray(std::move(subs))});
          return json::Value::fromObject(std::move(kc));
        } else {
          return std::nullopt;
        }
      },
      pc);
}

}  // namespace

auto TopicConfig::agentName() const -> std::string {
  if (const auto* p = std::get_if<AgentInboxConfig>(&parsed_config))
    return p->agent;
  if (const auto* p = std::get_if<TuiCommandsConfig>(&parsed_config))
    return p->agent;
  return {};
}

auto TopicConfig::subscribers() const -> const std::vector<std::string>& {
  if (const auto* p = std::get_if<PubsubConfig>(&parsed_config))
    return p->subscribers;
  static const std::vector<std::string> empty;
  return empty;
}

auto TopicConfig::toJson() const -> json::Value {
  std::map<std::string, json::Value> m;
  m.insert({"name", json::Value::from(name)});
  m.insert({"kind", json::Value::from(std::string{topicKindToStr(kind)})});
  if (auto kc = kindConfigToJson(parsed_config); kc.has_value()) {
    m.insert({"kind_config", std::move(*kc)});
  }
  return json::Value::fromObject(std::move(m));
}

auto TopicConfig::fromJson(const json::Value& v)
    -> Result<TopicConfig> {
  if (!v.isObject()) return std::unexpected{Error{"topic must be an object"}};
  TopicConfig cfg;
  cfg.name = v.getOrString("name");
  if (cfg.name.empty()) return std::unexpected{Error{"missing topic name"}};
  const auto kind_str = v.getOrString("kind");
  if (kind_str.empty()) return std::unexpected{Error{"missing topic kind"}};
  cfg.kind = topicKindFromStr(kind_str);
  cfg.parsed_config = kindConfigFromJson(cfg.kind, v.get("kind_config"));
  return cfg;
}

TopicRegistry::TopicRegistry(std::string path) : path_{std::move(path)} {}

auto TopicRegistry::load() -> Result<void> {
  std::ifstream in{path_};
  if (!in) return {};  // missing file ⇒ empty registry, success
  std::ostringstream buf;
  buf << in.rdbuf();
  const auto src = buf.str();
  if (src.empty()) return {};

  auto root = json::parse(src);
  if (!root) {
    return std::unexpected{Error{std::string{"parse topics.json: "} + root.error()}};
  }
  if (!root->isObject()) {
    return std::unexpected{Error{"topics.json must be an object"}};
  }
  const auto* topics = root->get("topics");
  if (topics == nullptr || !topics->isObject()) {
    return {};  // no topics field; empty registry is OK
  }
  for (const auto& [name, val] : topics->asObject()) {
    auto cfg = TopicConfig::fromJson(val);
    if (!cfg) {
      return std::unexpected{Error{std::string{"topic \""} + name +
                             "\": " + cfg.error().message}};
    }
    cfg->name = name;
    topics_.insert_or_assign(name, std::move(*cfg));
  }
  return {};
}

auto TopicRegistry::save() const -> Result<void> {
  std::error_code ec;
  std::filesystem::create_directories(
      std::filesystem::path{path_}.parent_path(), ec);

  std::map<std::string, json::Value> topics_obj;
  for (const auto& [name, cfg] : topics_) {
    topics_obj.insert({name, cfg.toJson()});
  }
  std::map<std::string, json::Value> root;
  root.insert(
      {"topics", json::Value::fromObject(std::move(topics_obj))});
  const auto wire = json::serialize(json::Value::fromObject(std::move(root)));

  const std::string tmp = path_ + ".tmp";
  std::ofstream out{tmp};
  if (!out) {
    return std::unexpected{Error{std::string{"open "} + tmp + " for write"}};
  }
  out << wire;
  out.close();
  if (!out) {
    return std::unexpected{Error{"write topics.json.tmp failed"}};
  }
  if (::rename(tmp.c_str(), path_.c_str()) != 0) {
    return std::unexpected{Error{std::string{"rename: "} + std::strerror(errno)}};
  }
  return {};
}

auto TopicRegistry::create(TopicConfig cfg)
    -> Result<void> {
  if (!isValidTopicName(cfg.name)) {
    return std::unexpected{Error{std::string{"invalid topic name \""} + cfg.name +
                           "\" (lowercase + digit + - / _ only)"}};
  }
  if (cfg.kind == TopicKind::Unknown) {
    return std::unexpected{Error{"missing or unrecognized topic kind"}};
  }
  if (topics_.contains(cfg.name)) {
    return std::unexpected{Error{std::string{"topic \""} + cfg.name +
                           "\" already exists"}};
  }
  topics_.insert_or_assign(cfg.name, std::move(cfg));
  if (auto r = save(); !r) return std::unexpected{r.error()};
  return {};
}

auto TopicRegistry::remove(std::string_view name) -> Result<void> {
  auto it = topics_.find(name);
  if (it == topics_.end()) {
    return std::unexpected{Error{std::string{"no such topic \""} +
                           std::string{name} + "\""}};
  }
  topics_.erase(it);
  if (auto r = save(); !r) return std::unexpected{r.error()};
  return {};
}

auto TopicRegistry::contains(std::string_view name) const -> bool {
  return topics_.find(name) != topics_.end();
}

auto TopicRegistry::get(std::string_view name) const -> const TopicConfig* {
  if (auto it = topics_.find(name); it != topics_.end()) return &it->second;
  return nullptr;
}

auto TopicRegistry::list() const -> std::vector<TopicConfig> {
  std::vector<TopicConfig> out;
  out.reserve(topics_.size());
  for (const auto& [_, cfg] : topics_) out.push_back(cfg);
  return out;
}

auto TopicRegistry::getOrAutoCreate(std::string_view name)
    -> Result<TopicConfig> {
  if (auto* existing = get(name); existing != nullptr) return *existing;
  if (!isValidTopicName(name)) {
    return std::unexpected{Error{std::string{"invalid topic name \""} +
                           std::string{name} + "\""}};
  }
  // Reject auto-creating a topic whose derived agent is ITSELF a topic
  // name (begins with inbox-/commands-). This is the only place malformed
  // nested-prefix topics are born: a caller that passes a topic name where
  // an agent name is expected — e.g. `bus msg mail inbox-human` does
  // "inbox-" + "inbox-human" — would otherwise silently create
  // `inbox-inbox-human` with agent "inbox-human". Fail loudly instead.
  const auto rejectNested = [&](std::string_view agent) -> bool {
    return agent.starts_with("inbox-") || agent.starts_with("commands-");
  };

  TopicConfig cfg;
  cfg.name = std::string{name};
  if (name.starts_with("inbox-")) {
    const auto agent = std::string{name.substr(6)};
    if (rejectNested(agent)) {
      return std::unexpected{Error{
          std::string{"refusing to auto-create nested-prefix topic \""} +
          std::string{name} + "\" (agent \"" + agent +
          "\" is itself a topic name); pass a bare agent name"}};
    }
    cfg.kind = TopicKind::AgentInbox;
    cfg.parsed_config = AgentInboxConfig{agent};
  } else if (name.starts_with("commands-")) {
    const auto agent = std::string{name.substr(9)};
    if (rejectNested(agent)) {
      return std::unexpected{Error{
          std::string{"refusing to auto-create nested-prefix topic \""} +
          std::string{name} + "\" (agent \"" + agent +
          "\" is itself a topic name); pass a bare agent name"}};
    }
    cfg.kind = TopicKind::TuiCommands;
    cfg.parsed_config = TuiCommandsConfig{agent};
  } else {
    return std::unexpected{Error{
        std::string{"topic \""} + std::string{name} +
        "\" not declared; create it with `bus topic create`"}};
  }
  if (auto r = create(cfg); !r) return std::unexpected{r.error()};
  return cfg;
}

}  // namespace bus
