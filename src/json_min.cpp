#include "json_min.h"

#include "types.h"

#include <cctype>
#include <charconv>
#include <stdexcept>
#include <utility>

namespace bus::json {

namespace {

auto escapeString(std::string_view s) -> std::string {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          // Bare control bytes — emit as \u00XX. Rare in our schema.
          static constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out += kHex[(c >> 4) & 0xF];
          out += kHex[c & 0xF];
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

class Parser {
 public:
  explicit Parser(std::string_view src) : src_{src} {}

  auto parseValue() -> std::expected<Value, std::string> {
    // RAII depth guard — every nested container recurses through here.
    struct DepthGuard {
      int& d;
      explicit DepthGuard(int& dd) : d{dd} { ++d; }
      ~DepthGuard() { --d; }
    } guard{depth_};
    if (depth_ > kMaxDepth) {
      return std::unexpected{"max nesting depth exceeded"};
    }
    skipWs();
    if (atEnd()) return std::unexpected{"unexpected end of input"};
    const char c = peek();
    if (c == '{') return parseObject();
    if (c == '[') return parseArray();
    if (c == '"') return parseString();
    if (c == 't' || c == 'f') return parseBool();
    if (c == 'n') return parseNull();
    if (c == '-' || (c >= '0' && c <= '9')) return parseInt();
    return std::unexpected{std::string{"unexpected character '"} + c + "'"};
  }

  auto end() -> std::expected<void, std::string> {
    skipWs();
    if (!atEnd()) return std::unexpected{"trailing content after value"};
    return {};
  }

 private:
  std::string_view src_;
  std::size_t pos_{0};
  int depth_{0};
  // Recursion-depth cap. parseValue is the single funnel every nested
  // container passes through, so guarding it bounds mutual recursion
  // (parseValue→parseArray/parseObject→parseValue). Without this, a long
  // run of '[' on the local broker.sock recurses one frame per level and
  // blows the 8 MiB stack → SIGSEGV (broker-hardening CRIT #1). 256 is
  // ~25× the deepest real RPC payload, ~2000× below the crash threshold.
  static constexpr int kMaxDepth = 256;

  auto atEnd() const -> bool { return pos_ >= src_.size(); }
  auto peek() const -> char { return src_[pos_]; }
  auto take() -> char { return src_[pos_++]; }

  auto skipWs() -> void {
    while (!atEnd()) {
      const char c = peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  auto expect(char c) -> std::expected<void, std::string> {
    skipWs();
    if (atEnd() || peek() != c) {
      return std::unexpected{std::string{"expected '"} + c + "'"};
    }
    ++pos_;
    return {};
  }

  auto parseNull() -> std::expected<Value, std::string> {
    if (src_.substr(pos_, 4) != "null") {
      return std::unexpected{"expected null"};
    }
    pos_ += 4;
    return Value{};
  }

  auto parseBool() -> std::expected<Value, std::string> {
    if (src_.substr(pos_, 4) == "true") {
      pos_ += 4;
      return Value::from(true);
    }
    if (src_.substr(pos_, 5) == "false") {
      pos_ += 5;
      return Value::from(false);
    }
    return std::unexpected{"expected true or false"};
  }

  auto parseInt() -> std::expected<Value, std::string> {
    const std::size_t start = pos_;
    if (peek() == '-') ++pos_;
    while (!atEnd() && peek() >= '0' && peek() <= '9') ++pos_;
    if (pos_ == start || (pos_ == start + 1 && src_[start] == '-')) {
      return std::unexpected{"invalid number"};
    }
    std::int64_t v = 0;
    const auto* first = src_.data() + start;
    const auto* last = src_.data() + pos_;
    if (auto r = std::from_chars(first, last, v);
        r.ec != std::errc{} || r.ptr != last) {
      return std::unexpected{"number out of range or malformed"};
    }
    return Value::from(v);
  }

  auto parseString() -> std::expected<Value, std::string> {
    if (peek() != '"') return std::unexpected{"expected string"};
    ++pos_;
    std::string out;
    while (!atEnd()) {
      const char c = take();
      if (c == '"') return Value::from(std::move(out));
      if (c == '\\') {
        if (atEnd()) return std::unexpected{"truncated escape"};
        const char e = take();
        switch (e) {
          case '"':  out += '"';  break;
          case '\\': out += '\\'; break;
          case '/':  out += '/';  break;
          case 'n':  out += '\n'; break;
          case 'r':  out += '\r'; break;
          case 't':  out += '\t'; break;
          case 'b':  out += '\b'; break;
          case 'f':  out += '\f'; break;
          case 'u': {
            if (pos_ + 4 > src_.size()) {
              return std::unexpected{"truncated \\u escape"};
            }
            auto hexVal = [](char ch) -> int {
              if (ch >= '0' && ch <= '9') return ch - '0';
              if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
              if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
              return -1;
            };
            int codepoint = 0;
            for (int i = 0; i < 4; ++i) {
              int h = hexVal(src_[pos_ + i]);
              if (h < 0) return std::unexpected{"bad hex in \\u escape"};
              codepoint = (codepoint << 4) | h;
            }
            pos_ += 4;
            if (codepoint <= 0x7F) {
              out += static_cast<char>(codepoint);
            } else if (codepoint <= 0x7FF) {
              out += static_cast<char>(0xC0 | (codepoint >> 6));
              out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
              out += static_cast<char>(0xE0 | (codepoint >> 12));
              out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
              out += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
            break;
          }
          default:
            return std::unexpected{std::string{"unknown escape \\"} + e};
        }
        continue;
      }
      out += c;
    }
    return std::unexpected{"unterminated string"};
  }

  auto parseArray() -> std::expected<Value, std::string> {
    if (auto r = expect('['); !r) return std::unexpected{r.error()};
    std::vector<Value> out;
    skipWs();
    if (!atEnd() && peek() == ']') {
      ++pos_;
      return Value::fromArray(std::move(out));
    }
    for (;;) {
      auto v = parseValue();
      if (!v) return std::unexpected{v.error()};
      out.push_back(std::move(*v));
      skipWs();
      if (atEnd()) return std::unexpected{"unterminated array"};
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        return Value::fromArray(std::move(out));
      }
      return std::unexpected{"expected ',' or ']' in array"};
    }
  }

  auto parseObject() -> std::expected<Value, std::string> {
    if (auto r = expect('{'); !r) return std::unexpected{r.error()};
    std::map<std::string, Value> out;
    skipWs();
    if (!atEnd() && peek() == '}') {
      ++pos_;
      return Value::fromObject(std::move(out));
    }
    for (;;) {
      skipWs();
      auto k = parseString();
      if (!k) return std::unexpected{k.error()};
      if (auto r = expect(':'); !r) return std::unexpected{r.error()};
      auto v = parseValue();
      if (!v) return std::unexpected{v.error()};
      out.insert_or_assign(k->asString(), std::move(*v));
      skipWs();
      if (atEnd()) return std::unexpected{"unterminated object"};
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        return Value::fromObject(std::move(out));
      }
      return std::unexpected{"expected ',' or '}' in object"};
    }
  }
};

}  // namespace

auto Value::from(bool b) -> Value {
  Value v;
  v.type_ = Type::Bool;
  v.b_ = b;
  return v;
}
auto Value::from(std::int64_t i) -> Value {
  Value v;
  v.type_ = Type::Int;
  v.i_ = i;
  return v;
}
auto Value::from(std::string s) -> Value {
  Value v;
  v.type_ = Type::String;
  v.s_ = std::move(s);
  return v;
}
auto Value::from(const char* s) -> Value { return from(std::string{s}); }
auto Value::fromArray(std::vector<Value> v) -> Value {
  Value out;
  out.type_ = Type::Array;
  out.arr_ = std::make_shared<std::vector<Value>>(std::move(v));
  return out;
}
auto Value::fromObject(std::map<std::string, Value> m) -> Value {
  Value out;
  out.type_ = Type::Object;
  out.obj_ = std::make_shared<std::map<std::string, Value>>(std::move(m));
  return out;
}

namespace {
auto typeStr(Type t) -> std::string_view {
  switch (t) {
    case Type::Null:   return "null";
    case Type::Bool:   return "bool";
    case Type::Int:    return "int";
    case Type::String: return "string";
    case Type::Array:  return "array";
    case Type::Object: return "object";
  }
  return "unknown";
}
}  // namespace

auto Value::asBool() const -> bool {
  if (type_ != Type::Bool)
    bus::fatal(std::string{"json::Value::asBool on "} + std::string{typeStr(type_)});
  return b_;
}
auto Value::asInt() const -> std::int64_t {
  if (type_ != Type::Int)
    bus::fatal(std::string{"json::Value::asInt on "} + std::string{typeStr(type_)});
  return i_;
}
auto Value::asString() const -> const std::string& {
  if (type_ != Type::String)
    bus::fatal(std::string{"json::Value::asString on "} + std::string{typeStr(type_)});
  return s_;
}
auto Value::asArray() const -> const std::vector<Value>& {
  if (type_ != Type::Array)
    bus::fatal(std::string{"json::Value::asArray on "} + std::string{typeStr(type_)});
  return *arr_;
}
auto Value::asObject() const -> const std::map<std::string, Value>& {
  if (type_ != Type::Object)
    bus::fatal(std::string{"json::Value::asObject on "} + std::string{typeStr(type_)});
  return *obj_;
}

auto Value::get(std::string_view key) const -> const Value* {
  if (type_ != Type::Object || !obj_) return nullptr;
  if (auto it = obj_->find(std::string{key}); it != obj_->end()) {
    return &it->second;
  }
  return nullptr;
}
auto Value::getOrString(std::string_view key, std::string_view def) const
    -> std::string {
  if (const auto* v = get(key); v && v->isString()) return v->asString();
  return std::string{def};
}
auto Value::getOrInt(std::string_view key, std::int64_t def) const
    -> std::int64_t {
  if (const auto* v = get(key); v && v->isInt()) return v->asInt();
  return def;
}
auto Value::getOrBool(std::string_view key, bool def) const -> bool {
  if (const auto* v = get(key); v && v->isBool()) return v->asBool();
  return def;
}

auto serialize(const Value& v) -> std::string {
  switch (v.type()) {
    case Type::Null:
      return "null";
    case Type::Bool:
      return v.asBool() ? "true" : "false";
    case Type::Int:
      return std::to_string(v.asInt());
    case Type::String:
      return escapeString(v.asString());
    case Type::Array: {
      std::string out = "[";
      bool first = true;
      for (const auto& x : v.asArray()) {
        if (!first) out += ',';
        first = false;
        out += serialize(x);
      }
      out += ']';
      return out;
    }
    case Type::Object: {
      std::string out = "{";
      bool first = true;
      for (const auto& [k, x] : v.asObject()) {
        if (!first) out += ',';
        first = false;
        out += escapeString(k);
        out += ':';
        out += serialize(x);
      }
      out += '}';
      return out;
    }
  }
  return "null";
}

auto parse(std::string_view src) -> std::expected<Value, std::string> {
  Parser p{src};
  auto v = p.parseValue();
  if (!v) return std::unexpected{v.error()};
  if (auto r = p.end(); !r) return std::unexpected{r.error()};
  return v;
}

}  // namespace bus::json
