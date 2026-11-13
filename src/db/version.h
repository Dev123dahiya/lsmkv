// Which files exist, at which level, and how they change.
//
// A Version is an immutable snapshot of the file layout. Compaction does not
// edit it; it builds a new one and swaps it in. Readers take a shared_ptr to
// the current Version and then work without the lock, so a compaction that
// finishes mid-read cannot delete a file out from under them — the file is
// unlinked only when the last Version naming it is gone.
//
// Every change is written to the manifest, a log of VersionEdit records,
// before it is applied in memory. Recovery replays that log.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "lsmkv/iterator.h"
#include "lsmkv/options.h"
#include "lsmkv/status.h"
#include "table/table_cache.h"
#include "util/env.h"
#include "wal/log.h"

namespace lsmkv {

// Level 0 holds freshly flushed memtables, whose ranges overlap. Below it
// each level is sorted and non-overlapping, and ten times larger.
constexpr int kNumLevels = 7;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  std::string smallest;  // smallest internal key in the file
  std::string largest;   // largest internal key in the file
};

// One atomic change to the file layout.
class VersionEdit {
 public:
  void Clear();

  void SetLogNumber(uint64_t n) {
    has_log_number_ = true;
    log_number_ = n;
  }
  void SetNextFile(uint64_t n) {
    has_next_file_number_ = true;
    next_file_number_ = n;
  }
  void SetLastSequence(SequenceNumber n) {
    has_last_sequence_ = true;
    last_sequence_ = n;
  }
  void SetCompactPointer(int level, std::string_view key) {
    compact_pointers_.emplace_back(level, std::string(key));
  }

  void AddFile(int level, const FileMetaData& file) { new_files_.emplace_back(level, file); }
  void RemoveFile(int level, uint64_t number) { deleted_files_.emplace(level, number); }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);

 private:
  friend class VersionSet;

  uint64_t log_number_ = 0;
  uint64_t next_file_number_ = 0;
  SequenceNumber last_sequence_ = 0;
  bool has_log_number_ = false;
  bool has_next_file_number_ = false;
  bool has_last_sequence_ = false;

  std::vector<std::pair<int, std::string>> compact_pointers_;
  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

class Version {
 public:
  Version() = default;

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  // Looks up `key`, consulting level 0 newest-first and then one file per
  // level below it. Returns NotFound when no level holds the key.
  Status Get(const ReadOptions& options, const LookupKey& key, std::string* value,
             TableCache* cache) const;

  // Appends an iterator per level-0 file and one concatenating iterator per
  // deeper level.
  void AddIterators(const ReadOptions& options, TableCache* cache,
                    std::vector<std::unique_ptr<Iterator>>* out) const;

  const std::vector<FileMetaData>& LevelFiles(int level) const { return files_[level]; }

  uint64_t LevelBytes(int level) const;

  // Score above 1.0 means the level wants compacting. Level 0 is scored by
  // file count because each of its files has to be consulted on every read;
  // the rest by total size.
  double CompactionScore(int* level, const Options& options) const;

  std::string DebugSummary() const;

 private:
  friend class VersionSet;

  std::vector<FileMetaData> files_[kNumLevels];
};

// Describes one compaction: the files going in and the level they merge into.
struct Compaction {
  int level = 0;
  std::vector<FileMetaData> inputs[2];  // [0] = level, [1] = level + 1
  // True when the output goes to the deepest level holding data, which is
  // the only place a tombstone can safely be dropped.
  bool bottommost = false;

  bool IsTrivialMove() const {
    // A single file with nothing to merge into can be moved by renaming it
    // in the manifest rather than rewriting its contents.
    return inputs[0].size() == 1 && inputs[1].empty();
  }
};

class VersionSet {
 public:
  VersionSet(std::string dbname, const Options& options, Env* env);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Reads CURRENT and replays the manifest it names.
  Status Recover();

  // Starts a fresh database: writes a manifest and the CURRENT file.
  Status CreateNew(uint64_t log_number);

  // Writes `edit` to the manifest, then installs the new version. The caller
  // holds the database mutex.
  Status LogAndApply(VersionEdit* edit);

  std::shared_ptr<const Version> current() const {
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
  }

  // These counters are read by the background compaction thread while the
  // write path advances them, so they are atomic rather than guarded: there
  // is no invariant between them to protect, only tearing to avoid.
  uint64_t NewFileNumber() { return next_file_number_.fetch_add(1, std::memory_order_relaxed); }
  uint64_t NextFileNumber() const { return next_file_number_.load(std::memory_order_relaxed); }
  void ReuseFileNumber(uint64_t number) {
    // Only give the number back if nothing else has taken one since.
    uint64_t expected = number + 1;
    next_file_number_.compare_exchange_strong(expected, number, std::memory_order_relaxed);
  }

  uint64_t LogNumber() const { return log_number_.load(std::memory_order_acquire); }
  void SetLogNumber(uint64_t n) { log_number_.store(n, std::memory_order_release); }

  SequenceNumber LastSequence() const { return last_sequence_.load(std::memory_order_acquire); }
  void SetLastSequence(SequenceNumber s) { last_sequence_.store(s, std::memory_order_release); }

  uint64_t ManifestFileNumber() const {
    return manifest_file_number_.load(std::memory_order_acquire);
  }

  // Chooses the next compaction, or null when nothing needs it.
  std::unique_ptr<Compaction> PickCompaction();

  // A compaction covering `begin`..`end` at the shallowest level that holds
  // overlapping files, for CompactRange.
  std::unique_ptr<Compaction> CompactRange(std::string_view begin, std::string_view end);

  // Every file number currently referenced, so the rest can be deleted.
  void AddLiveFiles(std::set<uint64_t>* live) const;

  std::string LevelSummary() const;

 private:
  Status WriteSnapshot(log::Writer* writer);
  void Apply(const VersionEdit& edit, const Version& base, Version* out) const;
  void Finalize(Version* v) const;

  const std::string dbname_;
  const Options options_;
  Env* const env_;

  // Guards `current_` only, so a reader taking the current version never
  // waits behind a manifest sync.
  mutable std::mutex mu_;
  std::shared_ptr<const Version> current_;

  // Serialises manifest writes and the compaction cursor. A compaction
  // started by CompactRange runs on the caller's thread while the background
  // thread may be compacting too, so two LogAndApply calls can genuinely
  // overlap; without this they would interleave records in the manifest.
  mutable std::mutex write_mu_;

  std::atomic<uint64_t> next_file_number_{2};  // 1 is the first manifest
  std::atomic<uint64_t> manifest_file_number_{0};
  std::atomic<uint64_t> log_number_{0};
  std::atomic<SequenceNumber> last_sequence_{0};

  // Where the last compaction of each level stopped, so compaction works
  // round-robin across the key space instead of always from the start.
  // Guarded by write_mu_.
  std::string compact_pointer_[kNumLevels];

  std::unique_ptr<WritableFile> manifest_file_;
  std::unique_ptr<log::Writer> manifest_log_;
};

// Files in `files` that overlap [begin, end] by user key.
std::vector<FileMetaData> FilesOverlapping(const std::vector<FileMetaData>& files,
                                           std::string_view begin, std::string_view end,
                                           bool level_zero);

}  // namespace lsmkv
