#pragma once

// Minimal JSON for the broker's RPC. Covers what our schema needs:
// null, bool, integer, string, array, object. No floats, no Unicode
// escapes (we control both ends; bodies are passed through as raw
// strings with standard \"/\\ escaping).

#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bus::json {

enum class Type { Null, Bool, Int, String, Array, Object };

class Value {
 public:
  Value() = default;
  static auto null_() -> Value;
  static auto from(bool b) -> Value;
  static auto from(std::int64_t i) -> Value;
  static auto from(std::string s) -> Value;
  static auto from(const char* s) -> Value;
  static auto fromArray(std::vector<Value> v) -> Value;
  static auto fromObject(std::map<std::string, Value> m) -> Value;

  auto type() const -> Type { return type_; }
  auto isNull() const -> bool { return type_ == Type::Null; }
  auto isBool() const -> bool { return type_ == Type::Bool; }
  auto isInt() const -> bool { return type_ == Type::Int; }
  auto isString() const -> bool { return type_ == Type::String; }
  auto isArray() const -> bool { return type_ == Type::Array; }
  auto isObject() const -> bool { return type_ == Type::Object; }

  auto asBool() const -> bool;
  auto asInt() const -> std::int64_t;
  auto asString() const -> const std::string&;
  auto asArray() const -> const std::vector<Value>&;
  auto asObject() const -> const std::map<std::string, Value>&;
  auto asArray() -> std::vector<Value>&;
  auto asObject() -> std::map<std::string, Value>&;

  // Object accessors. `get(key)` returns nullptr on miss. `getOr*`
  // returns the value or the default when key absent / wrong type.
  auto get(std::string_view key) const -> const Value*;
  auto getOrString(std::string_view key,
                   std::string_view def = "") const -> std::string;
  auto getOrInt(std::string_view key, std::int64_t def = 0) const
      -> std::int64_t;
  auto getOrBool(std::string_view key, bool def = false) const -> bool;

 private:
  Type type_{Type::Null};
  bool b_{};
  std::int64_t i_{};
  std::string s_;
  // Heap-allocated to keep Value's size small even with std::vector/map.
  std::shared_ptr<std::vector<Value>> arr_;
  std::shared_ptr<std::map<std::string, Value>> obj_;
};

// Serialize to compact JSON (no whitespace). UTF-8 byte-clean: any
// 0x80+ bytes in strings pass through unchanged.
auto serialize(const Value& v) -> std::string;

// Parse one complete JSON document. The leading byte must be one of
// `{` `[` `"` `t` `f` `n` `-` `0-9`. Trailing whitespace OK; trailing
// content is an error.
auto parse(std::string_view src) -> std::expected<Value, std::string>;

// Helpers for the common "build a flat result object" pattern.
auto okResponse() -> Value;
auto okResponse(std::map<std::string, Value> extras) -> Value;
auto errorResponse(std::string_view msg) -> Value;

}  // namespace bus::json
