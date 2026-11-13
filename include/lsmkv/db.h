// lsmkv: an embedded ordered key-value store built as a log-structured merge
// tree.
//
// Writes go to a write-ahead log and an in-memory table. When that table
// fills it is frozen and written out as a sorted table file, and background
// compaction merges those files down through the levels. Reads consult the
// memtables and then each level in turn, stopping at the newest version of
// the key.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

// An opaque handle to a point in the write history. Obtained from
// DB::GetSnapshot and released with DB::ReleaseSnapshot; holding one keeps
// the versions it needs from being dropped by compaction.
class Snapshot {
 protected:
  virtual ~Snapshot() = default;
};

class DB {
 public:
  // Opens the database in directory `name`, creating it when the options
  // allow. Returns an error rather than a partially usable handle.
  static Status Open(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr);

  DB() = default;
  virtual ~DB() = default;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  virtual Status Put(const WriteOptions& options, std::string_view key, std::string_view value) = 0;

  // Removing a key that is not present is not an error: a delete records a
  // tombstone either way.
  virtual Status Delete(const WriteOptions& options, std::string_view key) = 0;

  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // Returns NotFound when the key is absent. `value` is untouched on error.
  virtual Status Get(const ReadOptions& options, std::string_view key, std::string* value) = 0;

  // An iterator over the database in key order. It holds a snapshot, so
  // writes made after it is created are not visible through it, and it must
  // be destroyed before the database.
  virtual std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) = 0;

  virtual const Snapshot* GetSnapshot() = 0;
  virtual void ReleaseSnapshot(const Snapshot* snapshot) = 0;

  // Compacts the key range into the deepest level, dropping overwritten and
  // deleted entries. Empty bounds mean the start or end of the key space.
  virtual Status CompactRange(std::string_view begin, std::string_view end) = 0;

  // Blocks until no compaction is running. Intended for tests and for
  // measuring a quiet database.
  virtual void WaitForCompaction() = 0;

  // Human-readable counters: per-level file counts and sizes, and totals for
  // reads, writes, flushes and compactions.
  virtual std::string Stats() const = 0;
};

// Deletes every file belonging to the database. Be certain: it is not
// reversible, and it removes files the database is not currently using too.
Status DestroyDB(const std::string& name, const Options& options);

// Compile-time version of the library.
constexpr int kMajorVersion = 0;
constexpr int kMinorVersion = 1;

}  // namespace lsmkv
