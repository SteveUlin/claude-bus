#pragma once

// The messaging layer: it gives meaning to the opaque bytes a log stores.
// An Envelope packs the messaging fields (sender, TTL, epoch, protocol,
// deliver_when, body) into one payload; whoever needs those semantics
// decodes it, and the storage layer beneath never does.
//
// Keep this layer out of the storage kernel: the kernel must not link
// messaging code, so anything that only reads or appends opaque records
// stays free of it. That separation is why this lives in its own unit.
//
// Encoding — envelope v1 (1 byte version header + packed fields):
//   version      (u8)         1    value 1
//   sender_len   (u8)         1
//   sender       (bytes)
//   protocol_len (u8)         1
//   protocol     (bytes)
//   ttl_ms       (u32 LE)     4
//   deliver_when (u8)         1    0=immediate, 1=idle
//   epoch        (u64 LE)     8
//   body_len     (u32 LE)     4
//   body         (bytes)
//
// decodeEnvelope is lenient on a short/garbled buffer: returns a best-effort /
// empty Envelope, never crashes.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bus::msg {

struct Envelope {
  std::string sender;
  std::string protocol{"text"};
  std::uint32_t ttl_ms{0};
  std::uint8_t deliver_when{0};  // 0=immediate, 1=idle
  std::uint64_t epoch{0};
  std::string body;
};

// Serialize an Envelope to bytes for use as a Journal payload.
auto encodeEnvelope(const Envelope& env) -> std::vector<std::byte>;

// Deserialize an Envelope from a Journal payload. Lenient: never throws or
// crashes on short/garbage input — returns a best-effort (possibly empty)
// Envelope.
auto decodeEnvelope(std::span<const std::byte> payload) -> Envelope;

}  // namespace bus::msg
