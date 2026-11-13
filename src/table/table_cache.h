// Two caches sit between the levels and the filesystem.
//
// BlockCache holds decoded data blocks so a repeated read does not go to
// disk. TableCache holds open table files with their index and filter
// blocks, so a lookup does not reopen and reparse a file it just used.
//
// Both hand out shared_ptr, so an entry evicted while an iterator is walking
// it stays alive until that iterator is done.
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "table/block.h"
#include "table/sstable.h"
#include "util/env.h"

namespace lsmkv {

class BlockCache {
 public:
  explicit BlockCache(size_t capacity_bytes) : capacity_(capacity_bytes) {}

  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  // Returns the cached block for (cache_id, offset), or null.
  std::shared_ptr<Block> Lookup(uint64_t cache_id, uint64_t offset);

  // Inserts and evicts least-recently-used entries until the total charge is
  // within capacity.
  void Insert(uint64_t cache_id, uint64_t offset, std::shared_ptr<Block> block);

  // Drops every block belonging to a table, called when the table is closed.
  void EraseTable(uint64_t cache_id);

  uint64_t hits() const;
  uint64_t misses() const;
  size_t charge() const;

 private:
  struct Key {
    uint64_t cache_id;
    uint64_t offset;
    bool operator==(const Key& other) const {
      return cache_id == other.cache_id && offset == other.offset;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      // Mixing before combining keeps offsets from colliding across tables.
      const uint64_t a = k.cache_id * 0x9e3779b97f4a7c15ull;
      return static_cast<size_t>(a ^ (k.offset + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2)));
    }
  };
  struct Entry {
    Key key;
    std::shared_ptr<Block> block;
    size_t charge;
  };

  void EvictLocked();

  mutable std::mutex mu_;
  const size_t capacity_;
  size_t charge_ = 0;
  std::list<Entry> lru_;  // front is most recently used
  std::unordered_map<Key, std::list<Entry>::iterator, KeyHash> index_;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

// Opens table files on demand and keeps the most recently used ones open.
class TableCache {
 public:
  TableCache(std::string dbname, const Options& options, Env* env, size_t capacity);
  ~TableCache();

  TableCache(const TableCache&) = delete;
  TableCache& operator=(const TableCache&) = delete;

  // Iterator over one table file. The returned iterator keeps the table
  // open for as long as it lives.
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options, uint64_t file_number,
                                        uint64_t file_size);

  // Point lookup in one table.
  Status Get(const ReadOptions& options, uint64_t file_number, uint64_t file_size,
             std::string_view key,
             const std::function<void(std::string_view, std::string_view)>& handle_result);

  // Closes a table and drops its blocks, called after compaction deletes the
  // file.
  void Evict(uint64_t file_number);

  BlockCache* block_cache() { return &block_cache_; }

  uint64_t opens() const;

 private:
  struct TableEntry {
    uint64_t file_number;
    std::unique_ptr<RandomAccessFile> file;
    std::unique_ptr<Table> table;
    uint64_t cache_id;
  };

  Status FindTable(uint64_t file_number, uint64_t file_size,
                   std::shared_ptr<TableEntry>* entry);

  const std::string dbname_;
  const Options options_;
  Env* const env_;
  const size_t capacity_;

  mutable std::mutex mu_;
  std::list<std::shared_ptr<TableEntry>> lru_;
  std::unordered_map<uint64_t, std::list<std::shared_ptr<TableEntry>>::iterator> index_;
  uint64_t next_cache_id_ = 1;
  uint64_t opens_ = 0;

  BlockCache block_cache_;
};

// Path helpers, shared with the recovery code.
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string LogFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string LockFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);

}  // namespace lsmkv
