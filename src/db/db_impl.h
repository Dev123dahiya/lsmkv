// The database: the write path, the read path, and the background thread
// that keeps the tree in shape.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "db/dbformat.h"
#include "db/version.h"
#include "lsmkv/db.h"
#include "memtable/memtable.h"
#include "table/table_cache.h"
#include "util/env.h"
#include "wal/log.h"

namespace lsmkv {

class DBImpl final : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status Open();

  Status Put(const WriteOptions& options, std::string_view key, std::string_view value) override;
  Status Delete(const WriteOptions& options, std::string_view key) override;
  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, std::string_view key, std::string* value) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;
  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;
  Status CompactRange(std::string_view begin, std::string_view end) override;
  void WaitForCompaction() override;
  std::string Stats() const override;

 private:
  struct SnapshotImpl;

  Status RecoverLogFile(uint64_t log_number, VersionEdit* edit, SequenceNumber* max_sequence);

  // Writes the frozen memtable out as a table file and installs it.
  Status CompactMemTable();
  Status WriteLevel0Table(MemTable* mem, VersionEdit* edit);

  Status DoCompaction(Compaction* c);

  // Removes files no version references any more.
  void RemoveObsoleteFiles();

  void BackgroundLoop();
  void MaybeScheduleCompaction();  // mu_ held

  // The oldest sequence any live snapshot needs, below which older versions
  // of a key can be dropped.
  SequenceNumber OldestSnapshot() const;  // mu_ held

  const Options options_;
  const std::string dbname_;
  Env* const env_;

  std::unique_ptr<FileLock> db_lock_;
  std::unique_ptr<TableCache> table_cache_;
  std::unique_ptr<VersionSet> versions_;

  mutable std::mutex mu_;
  std::condition_variable background_done_;
  std::condition_variable writers_can_proceed_;

  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;  // being flushed; null when there is none
  std::atomic<bool> has_imm_{false};

  std::unique_ptr<WritableFile> logfile_;
  std::unique_ptr<log::Writer> log_;
  // Read by the background thread when it decides which logs are obsolete,
  // while the write path may be rolling over to a new one.
  std::atomic<uint64_t> logfile_number_{0};

  std::set<uint64_t> pending_outputs_;  // table files being written right now

  // Snapshots, oldest first.
  std::map<SequenceNumber, int> snapshots_;
  std::deque<std::unique_ptr<SnapshotImpl>> snapshot_handles_;

  std::thread background_thread_;
  bool background_running_ = false;
  bool shutting_down_ = false;
  bool background_work_pending_ = false;
  Status background_error_;

  // Counters reported by Stats().
  struct Counters {
    uint64_t writes = 0;
    uint64_t reads = 0;
    uint64_t flushes = 0;
    uint64_t compactions = 0;
    uint64_t trivial_moves = 0;
    uint64_t bytes_written = 0;
    uint64_t bytes_compacted = 0;
    uint64_t keys_dropped = 0;
    uint64_t write_stalls = 0;
  };
  Counters counters_;
};

}  // namespace lsmkv
