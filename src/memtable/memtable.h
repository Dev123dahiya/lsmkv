// The in-memory half of the tree: a sorted table of recent writes, backed by
// a skip list over arena memory.
//
// A memtable is append-only. An overwrite or delete inserts a new entry with
// a higher sequence number rather than touching the old one, so a reader that
// started earlier keeps seeing a consistent view.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "db/dbformat.h"
#include "lsmkv/iterator.h"
#include "memtable/skiplist.h"
#include "util/arena.h"

namespace lsmkv {

class MemTable {
 public:
  MemTable();

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Adds one entry. The caller holds the write lock; several threads may
  // call Get concurrently with this.
  void Add(SequenceNumber seq, ValueType type, std::string_view key, std::string_view value);

  // Looks up the newest entry for `key.user_key()` at or below its sequence.
  // Returns false when the memtable holds no entry for the key at all;
  // returns true with a NotFound status when it holds a tombstone, which
  // stops the search rather than falling through to older tables.
  bool Get(const LookupKey& key, std::string* value, Status* s) const;

  // Arena bytes held, including the skip list nodes.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  size_t EntryCount() const { return table_.ApproximateCount(); }

  // Iterates internal keys in order. The iterator points into arena memory
  // and must not outlive the memtable.
  std::unique_ptr<Iterator> NewIterator() const;

  // Entries are stored as a single arena blob:
  //   varint32(internal_key_size) | user_key | tag | varint32(vsize) | value
  // The skip list holds a pointer to the start of each blob. Exposed because
  // the iterator in the .cpp is parameterised on it; not part of the public
  // surface, which is include/lsmkv.
  struct KeyComparator {
    int operator()(const char* a, const char* b) const;
  };

  using Table = SkipList<const char*, KeyComparator>;

 private:
  Arena arena_;
  Table table_;
};

}  // namespace lsmkv
