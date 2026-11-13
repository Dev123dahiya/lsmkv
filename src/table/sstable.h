// Writing and reading sorted table files.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "table/block.h"
#include "table/format.h"
#include "util/env.h"

namespace lsmkv {

class BlockCache;

// Builds one table file. Keys must be added in increasing internal key
// order; the builder relies on that to prefix-compress and to build the
// index.
class TableBuilder {
 public:
  TableBuilder(const Options& options, WritableFile* file);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  void Add(std::string_view key, std::string_view value);

  // Writes the filter, index and footer. Nothing may be added afterwards.
  Status Finish();

  // Abandons the table without finishing it. The file is left for the caller
  // to delete.
  void Abandon();

  uint64_t NumEntries() const { return num_entries_; }
  uint64_t FileSize() const { return offset_; }
  Status status() const { return status_; }

 private:
  void FlushDataBlock();
  // Writes a block plus its trailer and fills in `handle`.
  void WriteRawBlock(std::string_view contents, BlockHandle* handle);

  const Options options_;
  WritableFile* const file_;
  uint64_t offset_ = 0;
  Status status_;

  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::string last_key_;
  uint64_t num_entries_ = 0;
  bool closed_ = false;

  // An index entry is only written once the next block starts, so the
  // separator can be shortened against the next block's first key.
  bool pending_index_entry_ = false;
  BlockHandle pending_handle_;

  std::unique_ptr<class BloomFilterBuilder> filter_;
  std::string compressed_scratch_;
};

// A read-only handle on one table file.
class Table {
 public:
  // Reads the footer, index and filter. `file` must outlive the Table.
  static Status Open(const Options& options, RandomAccessFile* file, uint64_t file_size,
                     uint64_t cache_id, BlockCache* cache, std::unique_ptr<Table>* result);

  ~Table();

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) const;

  // Point lookup. Calls `handle_result` at most once, with the first entry
  // whose key is >= `key`. The bloom filter is consulted first, so a miss
  // usually costs no block read at all.
  Status InternalGet(const ReadOptions& options, std::string_view key,
                     const std::function<void(std::string_view k, std::string_view v)>&
                         handle_result) const;

  // Approximate file offset of `key`, used to size compactions.
  uint64_t ApproximateOffsetOf(std::string_view key) const;

  // Counters for the tests and the stats output.
  uint64_t filter_skips() const { return filter_skips_.load(std::memory_order_relaxed); }

 private:
  struct Rep;

  explicit Table(std::unique_ptr<Rep> rep);

  std::unique_ptr<Iterator> BlockReader(const ReadOptions& options,
                                        std::string_view index_value) const;

  std::unique_ptr<Rep> rep_;
  mutable std::atomic<uint64_t> filter_skips_{0};
};

// Reads one block, verifies its checksum and returns the uncompressed
// contents. Exposed for the table tests.
Status ReadBlockContents(RandomAccessFile* file, const ReadOptions& options,
                         const BlockHandle& handle, std::string* result);

}  // namespace lsmkv
