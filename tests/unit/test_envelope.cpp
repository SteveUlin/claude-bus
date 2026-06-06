#include "harness.h"
#include "envelope.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace bus::msg;

// ── encode / decode round-trips ───────────────────────────────────────────────

TEST(envelope_roundtrip_all_fields) {
  Envelope env;
  env.sender = "alice";
  env.protocol = "myproto";
  env.ttl_ms = 12345;
  env.deliver_when = 1;
  env.epoch = 0xDEADBEEFCAFEBABEULL;
  env.body = "hello world";

  const auto wire = encodeEnvelope(env);
  const auto got = decodeEnvelope(wire);

  CHECK_EQ(got.sender, std::string{"alice"});
  CHECK_EQ(got.protocol, std::string{"myproto"});
  CHECK_EQ(got.ttl_ms, std::uint32_t{12345});
  CHECK_EQ(got.deliver_when, std::uint8_t{1});
  CHECK_EQ(got.epoch, std::uint64_t{0xDEADBEEFCAFEBABEULL});
  CHECK_EQ(got.body, std::string{"hello world"});
}

TEST(envelope_roundtrip_empty_fields) {
  Envelope env;
  // all-default
  const auto wire = encodeEnvelope(env);
  const auto got = decodeEnvelope(wire);
  CHECK_EQ(got.sender, std::string{""});
  // protocol defaults to "text" in decodeEnvelope when encoded as ""
  CHECK_EQ(got.ttl_ms, std::uint32_t{0});
  CHECK_EQ(got.deliver_when, std::uint8_t{0});
  CHECK_EQ(got.epoch, std::uint64_t{0});
  CHECK_EQ(got.body, std::string{""});
}

TEST(envelope_roundtrip_binary_body) {
  Envelope env;
  env.sender = "bot";
  env.body = std::string{"\x00\x01\x02\xFF\xFE", 5};
  const auto wire = encodeEnvelope(env);
  const auto got = decodeEnvelope(wire);
  CHECK_EQ(got.body, env.body);
  CHECK_EQ(got.sender, std::string{"bot"});
}

// ── lenient decode on short / garbage input ───────────────────────────────────

TEST(decode_empty_payload_no_crash) {
  const auto got = decodeEnvelope(std::span<const std::byte>{});
  // Should return an empty envelope, not crash.
  CHECK_EQ(got.sender, std::string{""});
  CHECK_EQ(got.body, std::string{""});
}

TEST(decode_garbage_payload_no_crash) {
  const std::vector<std::byte> junk{
      std::byte{0xFF}, std::byte{0xFE}, std::byte{0xFD},
      std::byte{0xFC}, std::byte{0xFB}, std::byte{0xFA}};
  // version != 1 => returns empty envelope, no crash
  const auto got = decodeEnvelope(junk);
  CHECK_EQ(got.sender, std::string{""});
  CHECK_EQ(got.body, std::string{""});
}

TEST(decode_truncated_mid_field_no_crash) {
  Envelope env;
  env.sender = "sender";
  env.body = "body text";
  const auto wire = encodeEnvelope(env);
  // Truncate partway through the body field.
  for (std::size_t cut = 1; cut < wire.size(); ++cut) {
    const auto got = decodeEnvelope(std::span<const std::byte>{wire.data(), cut});
    // Must not crash; fields may be partial/empty.
    (void)got.sender;
    (void)got.body;
  }
}

// A wire buffer with a valid header + sender but an inflated body_len (claims
// more bytes than the payload supplies) must:
//   - not crash
//   - decode sender correctly (it precedes body_len in the wire layout)
//   - return an empty body (the short-read path)
TEST(decode_inflated_body_len_no_crash) {
  Envelope env;
  env.sender = "alice";
  env.body = "short";
  const auto wire = encodeEnvelope(env);

  // Locate the body_len field.  Wire layout (v1):
  //   [0]      version (1)
  //   [1]      sender_len
  //   [2..2+slen-1]  sender
  //   [2+slen] protocol_len
  //   [3+slen..3+slen+plen-1]  protocol
  //   [3+slen+plen..3+slen+plen+3]  ttl_ms (u32)
  //   [7+slen+plen]  deliver_when
  //   [8+slen+plen..15+slen+plen]  epoch (u64)
  //   [16+slen+plen..19+slen+plen] body_len (u32)
  const std::size_t slen = env.sender.size();
  const std::size_t plen = env.protocol.size();
  const std::size_t body_len_off = 1 + 1 + slen + 1 + plen + 4 + 1 + 8;

  // Overwrite body_len to claim 1000 bytes.
  auto tampered = wire;
  const std::uint32_t big = 1000;
  for (int i = 0; i < 4; ++i)
    tampered[body_len_off + i] = std::byte((big >> (i * 8)) & 0xFF);

  const auto got = decodeEnvelope(tampered);
  // No crash; sender decoded correctly before body_len was reached.
  CHECK_EQ(got.sender, std::string{"alice"});
  // body_len claims 1000 but only 5 bytes remain — short-read returns empty.
  CHECK_EQ(got.body, std::string{});
}
