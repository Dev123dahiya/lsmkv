// Little-endian fixed-width and LEB128 varint encoding for the on-disk
// formats. Every multi-byte field written to a file goes through here, so the
// files are byte-identical across platforms.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lsmkv {

static_assert(std::endian::native == std::endian::little,
              "on-disk integers are little-endian; a big-endian port needs byte swaps here");

inline void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  std::memcpy(buf, &value, sizeof(buf));
  dst->append(buf, sizeof(buf));
}

inline void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  std::memcpy(buf, &value, sizeof(buf));
  dst->append(buf, sizeof(buf));
}

inline uint32_t DecodeFixed32(const char* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint64_t DecodeFixed64(const char* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Number of bytes PutVarint64 will write for `v`.
inline int VarintLength(uint64_t v) {
  int len = 1;
  while (v >= 128) {
    v >>= 7;
    ++len;
  }
  return len;
}

inline void PutVarint32(std::string* dst, uint32_t v) {
  uint8_t buf[5];
  uint8_t* p = buf;
  while (v >= 128) {
    *p++ = static_cast<uint8_t>(v) | 0x80;
    v >>= 7;
  }
  *p++ = static_cast<uint8_t>(v);
  dst->append(reinterpret_cast<const char*>(buf), static_cast<size_t>(p - buf));
}

inline void PutVarint64(std::string* dst, uint64_t v) {
  uint8_t buf[10];
  uint8_t* p = buf;
  while (v >= 128) {
    *p++ = static_cast<uint8_t>(v) | 0x80;
    v >>= 7;
  }
  *p++ = static_cast<uint8_t>(v);
  dst->append(reinterpret_cast<const char*>(buf), static_cast<size_t>(p - buf));
}

// Decodes a varint from the front of `input` and advances it past the value.
// Returns false and leaves `input` untouched if the buffer ends mid-varint or
// the encoding runs longer than 64 bits can hold.
inline bool GetVarint64(std::string_view* input, uint64_t* value) {
  uint64_t result = 0;
  size_t i = 0;
  for (int shift = 0; shift <= 63; shift += 7, ++i) {
    if (i >= input->size()) return false;  // ran out mid-varint
    const auto byte = static_cast<uint8_t>((*input)[i]);
    if (byte & 0x80) {
      result |= (static_cast<uint64_t>(byte & 0x7f) << shift);
    } else {
      result |= (static_cast<uint64_t>(byte) << shift);
      *value = result;
      input->remove_prefix(i + 1);
      return true;
    }
  }
  return false;  // more than ten bytes: not a 64-bit varint
}

inline bool GetVarint32(std::string_view* input, uint32_t* value) {
  uint64_t wide = 0;
  std::string_view copy = *input;
  if (!GetVarint64(&copy, &wide) || wide > UINT32_MAX) return false;
  *value = static_cast<uint32_t>(wide);
  *input = copy;
  return true;
}

// Decodes a varint32 from `p`, reading only the bytes the varint occupies and
// never past it. Used where the buffer's remaining length is not known but
// the encoding is trusted, such as entries inside a memtable arena. Returns
// the byte after the varint, or nullptr if the encoding is malformed.
inline const char* GetVarint32Ptr(const char* p, uint32_t* value) {
  uint32_t result = 0;
  for (int shift = 0; shift <= 28; shift += 7) {
    const auto byte = static_cast<uint32_t>(static_cast<uint8_t>(*p++));
    if (byte & 0x80) {
      result |= (byte & 0x7f) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return nullptr;
}

inline void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

// On success `result` points into the same buffer as `input`; it does not own
// the bytes and is only valid while that buffer lives.
inline bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  uint32_t len = 0;
  std::string_view copy = *input;
  if (!GetVarint32(&copy, &len) || copy.size() < len) return false;
  *result = copy.substr(0, len);
  copy.remove_prefix(len);
  *input = copy;
  return true;
}

}  // namespace lsmkv
