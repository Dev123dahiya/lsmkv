// CRC-32C (Castagnoli, polynomial 0x1EDC6F41) as used by iSCSI, SCTP and
// most storage engines. Uses the SSE4.2 crc32 instruction when the CPU
// reports it at runtime, and a slicing-by-8 table otherwise.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lsmkv::crc32c {

// Extends `crc` with `data`. Pass 0 for a fresh checksum.
uint32_t Extend(uint32_t crc, const void* data, size_t n) noexcept;

inline uint32_t Value(const void* data, size_t n) noexcept { return Extend(0, data, n); }
inline uint32_t Value(std::string_view s) noexcept { return Extend(0, s.data(), s.size()); }

// True when the SSE4.2 instruction path is in use. Reported so tests can
// check both implementations agree rather than silently testing one.
bool HardwareAccelerated() noexcept;

// Same checksum computed with the portable table, regardless of CPU support.
uint32_t ExtendPortable(uint32_t crc, const void* data, size_t n) noexcept;

// A stored checksum is masked so that a block of zeroes does not checksum to
// zero, which would let a zero-filled region look valid.
constexpr uint32_t kMaskDelta = 0xa282ead8u;

inline uint32_t Mask(uint32_t crc) noexcept {
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

inline uint32_t Unmask(uint32_t masked) noexcept {
  const uint32_t rot = masked - kMaskDelta;
  return (rot >> 17) | (rot << 15);
}

}  // namespace lsmkv::crc32c
