// A sorted block of key/value entries, the unit of storage and caching
// inside a table file.
//
// Keys are prefix-compressed against the previous key, which is effective
// because keys in a block are sorted and usually share a prefix. Every
// `restart_interval` entries one key is stored in full and its offset
// recorded, so a seek can binary search the restart points instead of
// scanning from the start.
//
//   entry   := varint32(shared) varint32(non_shared) varint32(value_size)
//              key_delta[non_shared] value[value_size]
//   block   := entry* restarts[num_restarts] fixed32(num_restarts)
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/iterator.h"
#include "lsmkv/status.h"

namespace lsmkv {

// Orders keys within a block. Data and index blocks hold internal keys; the
// metaindex block holds plain names, which must not have a tag stripped off
// them. A function pointer keeps one Block implementation for both.
using BlockKeyComparator = int (*)(std::string_view, std::string_view);

int CompareBytewise(std::string_view a, std::string_view b);
int CompareInternal(std::string_view a, std::string_view b);

class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Keys must arrive in increasing order.
  void Add(std::string_view key, std::string_view value);

  // Appends the restart array and returns the finished block. The builder
  // must be Reset before it is used again.
  std::string_view Finish();

  void Reset();

  // Size the block would have if finished now, used to decide when to cut.
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

  // The last key added, needed to build the index entry for this block.
  std::string_view last_key() const { return last_key_; }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

class Block {
 public:
  // Takes ownership of `contents`. Returns Corruption if the restart array
  // does not fit, which is how a truncated or damaged block is rejected
  // before any iterator walks into it.
  static Status Create(std::string contents, BlockKeyComparator cmp,
                       std::unique_ptr<Block>* result);

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return data_.size(); }
  uint32_t NumRestarts() const { return num_restarts_; }

  std::unique_ptr<Iterator> NewIterator() const;

 private:
  class Iter;

  Block(std::string contents, BlockKeyComparator cmp, uint32_t num_restarts,
        uint32_t restart_offset)
      : data_(std::move(contents)),
        compare_(cmp),
        num_restarts_(num_restarts),
        restart_offset_(restart_offset) {}

  const std::string data_;
  const BlockKeyComparator compare_;
  const uint32_t num_restarts_;
  const uint32_t restart_offset_;  // where the restart array begins
};

}  // namespace lsmkv
