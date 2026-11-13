#include "util/bloom.h"

#include <cstring>

namespace lsmkv {

// MurmurHash-like finaliser over 4-byte words. Only needs to spread bits
// well; it is not used for anything that must stay stable across versions
// other than the filter block, which is rewritten whenever a table is.
uint32_t BloomHash(std::string_view key) noexcept {
  constexpr uint32_t kSeed = 0xbc9f1d34;
  constexpr uint32_t m = 0xc6a4a793;
  const char* data = key.data();
  size_t n = key.size();

  uint32_t h = kSeed ^ static_cast<uint32_t>(n * m);
  while (n >= 4) {
    uint32_t w;
    std::memcpy(&w, data, sizeof(w));
    h += w;
    h *= m;
    h ^= (h >> 16);
    data += 4;
    n -= 4;
  }
  switch (n) {
    case 3:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16;
      [[fallthrough]];
    case 2:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8;
      [[fallthrough]];
    case 1:
      h += static_cast<uint32_t>(static_cast<uint8_t>(data[0]));
      h *= m;
      h ^= (h >> 24);
      break;
    default:
      break;
  }
  return h;
}

void BloomFilterBuilder::Finish(std::string* dst) {
  const size_t n = hashes_.size();
  size_t bits = n * static_cast<size_t>(bits_per_key_);
  // A very small filter has a high false positive rate; round up so the
  // saving is real rather than nominal.
  if (bits < 64) bits = 64;
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;

  const size_t init_size = dst->size();
  dst->resize(init_size + bytes, '\0');
  dst->push_back(static_cast<char>(probes_));

  char* array = dst->data() + init_size;
  for (const uint32_t base : hashes_) {
    // Rotating by 17 bits gives a second, effectively independent value.
    uint32_t h = base;
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int i = 0; i < probes_; ++i) {
      const size_t bitpos = h % bits;
      array[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      h += delta;
    }
  }
}

bool BloomMayContain(std::string_view filter, std::string_view key) noexcept {
  if (filter.size() < 2) return true;  // no filter, or malformed: read the block

  const size_t bytes = filter.size() - 1;
  const int probes = static_cast<uint8_t>(filter[bytes]);
  // Probe counts above 30 are reserved for future encodings; treat as "maybe"
  // rather than guessing at a format this build does not know.
  if (probes > 30) return true;

  const size_t bits = bytes * 8;
  const char* array = filter.data();

  uint32_t h = BloomHash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int i = 0; i < probes; ++i) {
    const size_t bitpos = h % bits;
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return false;
    h += delta;
  }
  return true;
}

}  // namespace lsmkv
