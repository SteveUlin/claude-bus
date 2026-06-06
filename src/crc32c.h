#pragma once

// CRC32C (Castagnoli) — software, table-driven, zero-dependency. Used as
// the per-record integrity check in the topic-log wire format (v5): a
// torn or corrupt record is detected on read and treated as logical EOF.
//
// A hardware path exists (SSE4.2 _mm_crc32_u64 on x86-64, the ARMv8 CRC
// extension on aarch64) and is ~10x faster, but the broker's record
// volume is tiny and software keeps this portable across every host the
// fleet runs on — so we use the table version unconditionally.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace bus {

// Reflected Castagnoli polynomial; standard init/xorout of 0xFFFFFFFF.
inline auto Crc32c(std::span<const std::byte> data) -> std::uint32_t {
  // static local in an inline function: C++17 guarantees one shared instance
  // across all TUs; the init-lambda runs at most once.
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();

  std::uint32_t crc = 0xFFFFFFFFu;
  for (const std::byte b : data) {
    crc = table[(crc ^ std::to_integer<std::uint8_t>(b)) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

}  // namespace bus
