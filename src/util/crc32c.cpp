#include "util/crc32c.h"

#include <array>
#include <bit>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#define LSMKV_X86 1
#include <nmmintrin.h>
#endif

namespace lsmkv::crc32c {
namespace {

// Reflected form of the Castagnoli polynomial.
constexpr uint32_t kPoly = 0x82f63b78u;

using Table = std::array<std::array<uint32_t, 256>, 8>;

// Slicing-by-8 tables, built at compile time so there is no literal table to
// transcribe and no initialisation order to worry about.
constexpr Table BuildTable() {
  Table t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (kPoly & (~(crc & 1) + 1));
    }
    t[0][i] = crc;
  }
  for (uint32_t i = 0; i < 256; ++i) {
    for (size_t slice = 1; slice < 8; ++slice) {
      const uint32_t prev = t[slice - 1][i];
      t[slice][i] = (prev >> 8) ^ t[0][prev & 0xff];
    }
  }
  return t;
}

constexpr Table kTable = BuildTable();

inline uint32_t Load32(const uint8_t* p) noexcept {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

#ifdef LSMKV_X86
bool DetectSse42() noexcept {
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2");
}

const bool kHasSse42 = DetectSse42();

__attribute__((target("sse4.2"))) uint32_t ExtendSse42(uint32_t crc, const uint8_t* p,
                                                      size_t n) noexcept {
  uint32_t c = ~crc;
  // Align to 8 bytes so the 64-bit instruction does the bulk of the work.
  while (n > 0 && (reinterpret_cast<uintptr_t>(p) & 7) != 0) {
    c = _mm_crc32_u8(c, *p++);
    --n;
  }
  uint64_t c64 = c;
  while (n >= 8) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    c64 = _mm_crc32_u64(c64, v);
    p += 8;
    n -= 8;
  }
  c = static_cast<uint32_t>(c64);
  while (n-- > 0) c = _mm_crc32_u8(c, *p++);
  return ~c;
}
#endif  // LSMKV_X86

}  // namespace

uint32_t ExtendPortable(uint32_t crc, const void* data, size_t n) noexcept {
  static_assert(std::endian::native == std::endian::little,
                "the slicing-by-8 loop reads little-endian words");
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = ~crc;

  while (n >= 8) {
    c ^= Load32(p);
    const uint32_t next = Load32(p + 4);
    c = kTable[7][c & 0xff] ^ kTable[6][(c >> 8) & 0xff] ^ kTable[5][(c >> 16) & 0xff] ^
        kTable[4][c >> 24] ^ kTable[3][next & 0xff] ^ kTable[2][(next >> 8) & 0xff] ^
        kTable[1][(next >> 16) & 0xff] ^ kTable[0][next >> 24];
    p += 8;
    n -= 8;
  }
  while (n-- > 0) {
    c = (c >> 8) ^ kTable[0][(c ^ *p++) & 0xff];
  }
  return ~c;
}

uint32_t Extend(uint32_t crc, const void* data, size_t n) noexcept {
#ifdef LSMKV_X86
  if (kHasSse42) return ExtendSse42(crc, static_cast<const uint8_t*>(data), n);
#endif
  return ExtendPortable(crc, data, n);
}

bool HardwareAccelerated() noexcept {
#ifdef LSMKV_X86
  return kHasSse42;
#else
  return false;
#endif
}

}  // namespace lsmkv::crc32c
