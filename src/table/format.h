// On-disk layout of a sorted table file.
//
//   table := data_block*  filter_block?  metaindex_block  index_block  footer
//
// Every block on disk is followed by a one-byte compression type and a
// four-byte masked CRC32C covering the block and that byte:
//
//   block_on_disk := contents | compression (1B) | crc (4B)
//
// The footer is a fixed 48 bytes at the end of the file so a reader can find
// the index without scanning:
//
//   footer := metaindex_handle | index_handle | padding | magic (8B)
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

// "lsmkv tbl" hashed down to eight bytes; a file that does not end with this
// is not one of ours, which is a clearer error than a parse failure.
constexpr uint64_t kTableMagicNumber = 0x6c736d6b76746231ull;

constexpr size_t kBlockTrailerSize = 5;  // compression byte + CRC32C

enum class CompressionType : uint8_t {
  kNone = 0x0,
  // Reserved so a file written by a future build that adds compression is
  // rejected with a clear message rather than misread.
  kSnappy = 0x1,
  kZstd = 0x2,
};

// Position and length of a block within a file.
class BlockHandle {
 public:
  // Two varint64s, each at most ten bytes.
  static constexpr size_t kMaxEncodedLength = 20;

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }
  void set_offset(uint64_t v) { offset_ = v; }
  void set_size(uint64_t v) { size_ = v; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view* input);

 private:
  uint64_t offset_ = 0;
  uint64_t size_ = 0;
};

class Footer {
 public:
  // Two handles padded to a fixed width, plus the magic number.
  static constexpr size_t kEncodedLength = 2 * BlockHandle::kMaxEncodedLength + 8;

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  const BlockHandle& index_handle() const { return index_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

}  // namespace lsmkv
