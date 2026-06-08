#include "envelope.h"

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus::msg {

namespace {

auto putU8(std::vector<std::byte>& b, std::uint8_t v) -> void {
  b.push_back(std::byte{v});
}
auto putU32(std::vector<std::byte>& b, std::uint32_t v) -> void {
  for (int i = 0; i < 4; ++i) b.push_back(std::byte((v >> (i * 8)) & 0xFF));
}
auto putU64(std::vector<std::byte>& b, std::uint64_t v) -> void {
  for (int i = 0; i < 8; ++i) b.push_back(std::byte((v >> (i * 8)) & 0xFF));
}
auto putBytes(std::vector<std::byte>& b, std::string_view s) -> void {
  for (char c : s) b.push_back(std::byte(static_cast<std::uint8_t>(c)));
}

}  // namespace

auto encodeEnvelope(const Envelope& env) -> std::vector<std::byte> {
  // sender and protocol are length-prefixed with a u8 field — 255 bytes max.
  // Callers must not exceed this limit; violating this is a programmer error.
  if (env.sender.size() > 0xFF)
    bus::fatal("encodeEnvelope: sender exceeds 255 bytes");
  if (env.protocol.size() > 0xFF)
    bus::fatal("encodeEnvelope: protocol exceeds 255 bytes");
  const auto slen = static_cast<std::uint8_t>(env.sender.size());
  const auto plen = static_cast<std::uint8_t>(env.protocol.size());

  std::vector<std::byte> buf;
  buf.reserve(1 + 1 + slen + 1 + plen + 4 + 1 + 8 + 4 + env.body.size());

  putU8(buf, 1);  // envelope version 1
  putU8(buf, slen);
  putBytes(buf, std::string_view{env.sender}.substr(0, slen));
  putU8(buf, plen);
  putBytes(buf, std::string_view{env.protocol}.substr(0, plen));
  putU32(buf, env.ttl_ms);
  putU8(buf, env.deliver_when);
  putU64(buf, env.epoch);
  putU32(buf, static_cast<std::uint32_t>(env.body.size()));
  putBytes(buf, env.body);

  return buf;
}

auto decodeEnvelope(std::span<const std::byte> payload) -> Envelope {
  Envelope env;
  const std::size_t sz = payload.size();
  // Lenient: on any short-read advance pos to EOF so subsequent field reads
  // predictably return zero/empty rather than reading from a stale offset.
  auto getBytes = [&](std::size_t& pos, std::size_t n) -> std::string {
    if (pos + n > sz) { pos = sz; return {}; }
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
      s[i] = static_cast<char>(std::to_integer<std::uint8_t>(payload[pos + i]));
    pos += n;
    return s;
  };
  auto getU8 = [&](std::size_t& pos) -> std::uint8_t {
    if (pos >= sz) return 0;
    return std::to_integer<std::uint8_t>(payload[pos++]);
  };
  auto getU32le = [&](std::size_t& pos) -> std::uint32_t {
    if (pos + 4 > sz) { pos = sz; return 0; }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v |= static_cast<std::uint32_t>(
               std::to_integer<std::uint8_t>(payload[pos + i])) << (i * 8);
    pos += 4;
    return v;
  };
  auto getU64le = [&](std::size_t& pos) -> std::uint64_t {
    if (pos + 8 > sz) { pos = sz; return 0; }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v |= static_cast<std::uint64_t>(
               std::to_integer<std::uint8_t>(payload[pos + i])) << (i * 8);
    pos += 8;
    return v;
  };

  std::size_t pos = 0;
  const auto ver = getU8(pos);
  if (ver != 1) return env;  // unknown version — return empty

  const auto slen = getU8(pos);
  env.sender = getBytes(pos, slen);

  const auto plen = getU8(pos);
  env.protocol = getBytes(pos, plen);
  if (env.protocol.empty()) env.protocol = "text";

  env.ttl_ms = getU32le(pos);
  env.deliver_when = getU8(pos);
  env.epoch = getU64le(pos);

  const auto blen = getU32le(pos);
  env.body = getBytes(pos, blen);

  return env;
}

}  // namespace bus::msg
