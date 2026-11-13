// The internal key format shared by the memtable, the log and the tables.
//
// Every write — including a delete — is stored as a new record stamped with a
// monotonically increasing sequence number. Nothing is overwritten in place,
// which is what makes snapshots and crash recovery possible: a reader at
// sequence N sees exactly the writes with sequence <= N.
//
//   internal key := user_key | fixed64(sequence << 8 | type)
//
// Keys sort by user key ascending, then by sequence descending, so the newest
// version of a key is the first one an iterator meets.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "util/coding.h"

namespace lsmkv {

using SequenceNumber = uint64_t;

// The tag packs the type into the low byte, so the sequence has 56 bits.
constexpr SequenceNumber kMaxSequenceNumber = (1ull << 56) - 1;

enum class ValueType : uint8_t {
  kDeletion = 0x0,
  kValue = 0x1,
};

// When searching for a user key the lookup must land on the newest version,
// so it is stamped with the largest possible tag for that sequence.
constexpr ValueType kValueTypeForSeek = ValueType::kValue;

inline uint64_t PackTag(SequenceNumber seq, ValueType t) {
  return (seq << 8) | static_cast<uint8_t>(t);
}

struct ParsedInternalKey {
  std::string_view user_key;
  SequenceNumber sequence = 0;
  ValueType type = ValueType::kValue;
};

inline void AppendInternalKey(std::string* result, const ParsedInternalKey& key) {
  result->append(key.user_key);
  PutFixed64(result, PackTag(key.sequence, key.type));
}

// Returns false when the key is too short to hold a tag or carries a type
// this build does not define, both of which mean the file is corrupt.
inline bool ParseInternalKey(std::string_view internal_key, ParsedInternalKey* result) {
  if (internal_key.size() < 8) return false;
  const uint64_t tag = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
  const auto type = static_cast<uint8_t>(tag & 0xff);
  if (type > static_cast<uint8_t>(ValueType::kValue)) return false;
  result->user_key = internal_key.substr(0, internal_key.size() - 8);
  result->sequence = tag >> 8;
  result->type = static_cast<ValueType>(type);
  return true;
}

// Strips the tag. The caller must already know the key is well formed; this
// is on the hot path of every comparison.
inline std::string_view ExtractUserKey(std::string_view internal_key) {
  return internal_key.substr(0, internal_key.size() - 8);
}

inline SequenceNumber ExtractSequence(std::string_view internal_key) {
  return DecodeFixed64(internal_key.data() + internal_key.size() - 8) >> 8;
}

// Orders by user key ascending, then tag descending so that the newest
// version of a user key sorts first. Returns <0, 0 or >0.
inline int CompareInternalKey(std::string_view a, std::string_view b) {
  const int r = ExtractUserKey(a).compare(ExtractUserKey(b));
  if (r != 0) return r;
  const uint64_t atag = DecodeFixed64(a.data() + a.size() - 8);
  const uint64_t btag = DecodeFixed64(b.data() + b.size() - 8);
  if (atag > btag) return -1;
  if (atag < btag) return 1;
  return 0;
}

// Shortest string >= `start` and < `limit`, used to keep index entries small.
// Returning `start` unchanged is always correct, just less compact.
// These two operate on plain byte strings — user keys, not internal keys.
std::string ShortestSeparator(std::string_view start, std::string_view limit);

// Shortest string > `key`, for the last index entry in a table.
std::string ShortSuccessor(std::string_view key);

// The same two for internal keys.
//
// Shortening an internal key with the plain versions would cut into its
// 8-byte tag and produce something that is not an internal key at all, which
// then compares as garbage. These shorten the user key and re-stamp it with
// the largest possible tag, so the result still sorts strictly between the
// two keys it separates.
std::string ShortestInternalSeparator(std::string_view start, std::string_view limit);
std::string ShortInternalSuccessor(std::string_view key);

// Carries the three encodings a lookup needs without three allocations: the
// memtable wants a length-prefixed internal key, the tables want the bare
// internal key, and the bloom filter wants the user key.
class LookupKey {
 public:
  LookupKey(std::string_view user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;

  std::string_view memtable_key() const { return {start_, static_cast<size_t>(end_ - start_)}; }
  std::string_view internal_key() const {
    return {kstart_, static_cast<size_t>(end_ - kstart_)};
  }
  std::string_view user_key() const {
    return {kstart_, static_cast<size_t>(end_ - kstart_ - 8)};
  }

 private:
  // Layout: varint32(klength) | user_key | tag
  //         ^start_            ^kstart_          ^end_
  // Short keys avoid the heap entirely.
  char space_[200];
  std::string heap_;
  const char* start_;
  const char* kstart_;
  const char* end_;
};

}  // namespace lsmkv
