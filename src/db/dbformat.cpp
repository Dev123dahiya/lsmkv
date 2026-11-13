#include "db/dbformat.h"

#include <algorithm>
#include <cstring>

namespace lsmkv {

std::string ShortestSeparator(std::string_view start, std::string_view limit) {
  const size_t min_length = std::min(start.size(), limit.size());
  size_t diff_index = 0;
  while (diff_index < min_length && start[diff_index] == limit[diff_index]) ++diff_index;

  std::string result(start);
  if (diff_index >= min_length) {
    // One is a prefix of the other; no shorter separator exists.
    return result;
  }
  const auto diff_byte = static_cast<uint8_t>(start[diff_index]);
  if (diff_byte < 0xff && diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
    result[diff_index] = static_cast<char>(diff_byte + 1);
    result.resize(diff_index + 1);
  }
  return result;
}

std::string ShortSuccessor(std::string_view key) {
  // Find the first byte that can be incremented and drop everything after it.
  for (size_t i = 0; i < key.size(); ++i) {
    const auto byte = static_cast<uint8_t>(key[i]);
    if (byte != 0xff) {
      std::string result(key.substr(0, i + 1));
      result[i] = static_cast<char>(byte + 1);
      return result;
    }
  }
  // All 0xff: nothing shorter is larger, so keep the key as it is.
  return std::string(key);
}

std::string ShortestInternalSeparator(std::string_view start, std::string_view limit) {
  const std::string_view user_start = ExtractUserKey(start);
  const std::string_view user_limit = ExtractUserKey(limit);
  const std::string shortened = ShortestSeparator(user_start, user_limit);

  if (shortened.size() < user_start.size() && user_start.compare(shortened) < 0) {
    // `shortened` sorts strictly after every key beginning with user_start,
    // and strictly before user_limit. Stamping it with the largest tag puts
    // it ahead of any real key that shares it, so it still separates the two
    // blocks correctly while being shorter than either.
    std::string result = shortened;
    PutFixed64(&result, PackTag(kMaxSequenceNumber, kValueTypeForSeek));
    return result;
  }
  // Nothing shorter exists; the original is always a valid separator.
  return std::string(start);
}

std::string ShortInternalSuccessor(std::string_view key) {
  const std::string_view user_key = ExtractUserKey(key);
  const std::string shortened = ShortSuccessor(user_key);
  if (shortened.size() < user_key.size() || user_key.compare(shortened) < 0) {
    std::string result = shortened;
    PutFixed64(&result, PackTag(kMaxSequenceNumber, kValueTypeForSeek));
    return result;
  }
  return std::string(key);
}

LookupKey::LookupKey(std::string_view user_key, SequenceNumber sequence) {
  const size_t usize = user_key.size();
  const size_t needed = usize + 13;  // varint32 (<=5) + key + tag (8)

  char* dst;
  if (needed > sizeof(space_)) {
    heap_.resize(needed);
    dst = heap_.data();
  } else {
    dst = space_;
  }
  start_ = dst;

  // Hand-rolled varint32 so the three pointers can be set as we go.
  auto klength = static_cast<uint32_t>(usize + 8);
  while (klength >= 128) {
    *dst++ = static_cast<char>(static_cast<uint8_t>(klength) | 0x80);
    klength >>= 7;
  }
  *dst++ = static_cast<char>(klength);

  kstart_ = dst;
  std::memcpy(dst, user_key.data(), usize);
  dst += usize;
  const uint64_t tag = PackTag(sequence, kValueTypeForSeek);
  std::memcpy(dst, &tag, sizeof(tag));
  dst += sizeof(tag);
  end_ = dst;
}

}  // namespace lsmkv
