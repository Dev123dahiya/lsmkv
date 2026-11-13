#include "db/db_impl.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "db/write_batch_internal.h"
#include "table/iterators.h"
#include "table/sstable.h"
#include "util/coding.h"

namespace lsmkv {

struct DBImpl::SnapshotImpl : public Snapshot {
  explicit SnapshotImpl(SequenceNumber s) : sequence(s) {}
  ~SnapshotImpl() override = default;
  SequenceNumber sequence;
};

namespace {

// Presents the merged internal stream as user keys at one snapshot: newest
// visible version of each key, tombstones removed.
class DBIter final : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> internal, SequenceNumber sequence)
      : iter_(std::move(internal)), sequence_(sequence) {}

  bool Valid() const override { return valid_; }
  std::string_view key() const override { return saved_key_; }
  std::string_view value() const override {
    return direction_ == Direction::kForward ? iter_->value() : std::string_view(saved_value_);
  }
  Status status() const override { return status_.ok() ? iter_->status() : status_; }

  void SeekToFirst() override {
    direction_ = Direction::kForward;
    saved_value_.clear();
    iter_->SeekToFirst();
    if (iter_->Valid()) {
      FindNextUserEntry(false, &saved_key_);
    } else {
      valid_ = false;
    }
  }

  void SeekToLast() override {
    direction_ = Direction::kReverse;
    saved_value_.clear();
    iter_->SeekToLast();
    FindPrevUserEntry();
  }

  void Seek(std::string_view target) override {
    direction_ = Direction::kForward;
    saved_value_.clear();
    saved_key_.clear();
    // Seek to the newest possible version of the target, which sorts first
    // among that user key's entries.
    AppendInternalKey(&saved_key_, {target, sequence_, kValueTypeForSeek});
    iter_->Seek(saved_key_);
    if (iter_->Valid()) {
      FindNextUserEntry(false, &saved_key_);
    } else {
      valid_ = false;
    }
  }

  void Next() override {
    assert(valid_);
    if (direction_ == Direction::kReverse) {
      // Was walking backwards: the underlying iterator sits before the
      // current key, so step it forward past it first.
      direction_ = Direction::kForward;
      if (!iter_->Valid()) {
        iter_->SeekToFirst();
      } else {
        iter_->Next();
      }
      if (!iter_->Valid()) {
        valid_ = false;
        saved_key_.clear();
        return;
      }
    } else {
      // Remember the key being left behind, so every older version of it is
      // skipped rather than returned again.
      saved_key_.assign(ExtractUserKey(iter_->key()));
    }
    FindNextUserEntry(true, &saved_key_);
  }

  void Prev() override {
    assert(valid_);
    if (direction_ == Direction::kForward) {
      // Was walking forwards: step back until the underlying iterator is on
      // a different user key, then take the newest version of that one.
      const std::string current(ExtractUserKey(iter_->key()));
      while (true) {
        iter_->Prev();
        if (!iter_->Valid()) {
          valid_ = false;
          saved_key_.clear();
          saved_value_.clear();
          return;
        }
        if (ExtractUserKey(iter_->key()) != current) break;
      }
      direction_ = Direction::kReverse;
    }
    FindPrevUserEntry();
  }

 private:
  enum class Direction { kForward, kReverse };

  // Advances to the next visible, non-deleted entry. When `skipping` is set,
  // entries whose user key equals `skip` are passed over.
  void FindNextUserEntry(bool skipping, std::string* skip) {
    assert(iter_->Valid());
    assert(direction_ == Direction::kForward);
    do {
      ParsedInternalKey parsed;
      if (!ParseInternalKey(iter_->key(), &parsed)) {
        status_ = Status::Corruption("malformed internal key in the merged stream");
        valid_ = false;
        return;
      }
      if (parsed.sequence <= sequence_) {
        switch (parsed.type) {
          case ValueType::kDeletion:
            // Hide this key and every older version of it.
            skip->assign(parsed.user_key);
            skipping = true;
            break;
          case ValueType::kValue:
            if (skipping && parsed.user_key <= std::string_view(*skip)) break;  // older version
            valid_ = true;
            saved_key_.assign(parsed.user_key);
            return;
        }
      }
      iter_->Next();
    } while (iter_->Valid());
    saved_key_.clear();
    valid_ = false;
  }

  // Walks back to the newest visible version of the user key the underlying
  // iterator is currently inside.
  void FindPrevUserEntry() {
    assert(direction_ == Direction::kReverse);
    ValueType type = ValueType::kDeletion;

    if (iter_->Valid()) {
      do {
        ParsedInternalKey parsed;
        if (!ParseInternalKey(iter_->key(), &parsed)) {
          status_ = Status::Corruption("malformed internal key in the merged stream");
          valid_ = false;
          return;
        }
        if (parsed.sequence <= sequence_) {
          // Moving backwards, versions arrive oldest first, so each visible
          // one replaces the last until the user key changes.
          if (type != ValueType::kDeletion && parsed.user_key < std::string_view(saved_key_)) {
            break;  // the previous key is complete
          }
          type = parsed.type;
          if (type == ValueType::kDeletion) {
            saved_key_.clear();
            saved_value_.clear();
          } else {
            saved_key_.assign(parsed.user_key);
            saved_value_.assign(iter_->value());
          }
        }
        iter_->Prev();
      } while (iter_->Valid());
    }

    if (type == ValueType::kDeletion) {
      valid_ = false;  // ran off the start, or the key is deleted
      saved_key_.clear();
      saved_value_.clear();
      direction_ = Direction::kForward;
    } else {
      valid_ = true;
    }
  }

  std::unique_ptr<Iterator> iter_;
  const SequenceNumber sequence_;
  Direction direction_ = Direction::kForward;
  bool valid_ = false;
  std::string saved_key_;
  std::string saved_value_;
  Status status_;
};

}  // namespace

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options),
      dbname_(std::move(dbname)),
      env_(options.env != nullptr ? options.env : Env::Default()) {}

DBImpl::~DBImpl() {
  {
    std::unique_lock<std::mutex> lock(mu_);
    shutting_down_ = true;
    writers_can_proceed_.notify_all();
    background_done_.notify_all();
  }
  if (background_thread_.joinable()) background_thread_.join();

  // The log is closed before the lock is dropped so a reopen in the same
  // process sees a complete file.
  if (logfile_) logfile_->Close();
  logfile_.reset();
  log_.reset();
  versions_.reset();
  table_cache_.reset();
  db_lock_.reset();
}

Status DBImpl::Open() {
  LSMKV_TRY(env_->CreateDir(dbname_));
  LSMKV_TRY(env_->LockFile(LockFileName(dbname_), &db_lock_));

  table_cache_ = std::make_unique<TableCache>(dbname_, options_, env_, options_.max_open_files);
  versions_ = std::make_unique<VersionSet>(dbname_, options_, env_, table_cache_.get());

  const bool exists = env_->FileExists(CurrentFileName(dbname_));
  if (exists && options_.error_if_exists) {
    return Status::InvalidArgument("database already exists: " + dbname_);
  }
  if (!exists && !options_.create_if_missing) {
    return Status::NotFound("database does not exist: " + dbname_);
  }

  VersionEdit edit;
  SequenceNumber max_sequence = 0;

  if (exists) {
    LSMKV_TRY(versions_->Recover());

    // Replay every log newer than the one the manifest recorded. A log older
    // than that was already folded into a table before the crash.
    std::vector<std::string> children;
    LSMKV_TRY(env_->GetChildren(dbname_, &children));
    std::vector<uint64_t> logs;
    for (const auto& name : children) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
        const uint64_t number = std::strtoull(name.c_str(), nullptr, 10);
        if (number >= versions_->LogNumber()) logs.push_back(number);
      }
    }
    std::sort(logs.begin(), logs.end());
    for (const uint64_t number : logs) {
      LSMKV_TRY(RecoverLogFile(number, &edit, &max_sequence));
      // A file number seen in a log must not be handed out again.
      if (versions_->NextFileNumber() <= number) versions_->SetLastSequence(max_sequence);
    }
    if (max_sequence > versions_->LastSequence()) versions_->SetLastSequence(max_sequence);
  }

  // A fresh log for this session, whether the database is new or recovered.
  const uint64_t new_log_number = versions_->NewFileNumber();
  LSMKV_TRY(env_->NewWritableFile(LogFileName(dbname_, new_log_number), &logfile_));
  log_ = std::make_unique<log::Writer>(logfile_.get());
  logfile_number_.store(new_log_number, std::memory_order_release);
  mem_ = std::make_shared<MemTable>();

  if (!exists) {
    versions_->SetLogNumber(new_log_number);
    LSMKV_TRY(versions_->CreateNew(new_log_number));
  } else {
    edit.SetLogNumber(new_log_number);
    versions_->SetLogNumber(new_log_number);
    LSMKV_TRY(versions_->LogAndApply(&edit));
  }

  RemoveObsoleteFiles();

  {
    std::lock_guard<std::mutex> lock(mu_);
    background_thread_ = std::thread([this] { BackgroundLoop(); });
    MaybeScheduleCompaction();
  }
  return Status::OK();
}

namespace {

class LogReporter final : public log::Reader::Reporter {
 public:
  explicit LogReporter(bool paranoid) : paranoid_(paranoid) {}
  void Corruption(size_t, const Status& status) override {
    if (paranoid_ && first_.ok()) first_ = status;
  }
  const Status& first() const { return first_; }

 private:
  const bool paranoid_;
  Status first_;
};

}  // namespace

Status DBImpl::RecoverLogFile(uint64_t log_number, VersionEdit* edit,
                              SequenceNumber* max_sequence) {
  std::unique_ptr<SequentialFile> file;
  LSMKV_TRY(env_->NewSequentialFile(LogFileName(dbname_, log_number), &file));

  LogReporter reporter(options_.paranoid_checks);
  log::Reader reader(file.get(), &reporter, true, 0);

  auto mem = std::make_shared<MemTable>();
  std::string scratch;
  std::string_view record;
  uint64_t records = 0;

  while (reader.ReadRecord(&record, &scratch)) {
    if (record.size() < kWriteBatchHeaderSize) {
      return Status::Corruption("log record is shorter than a write batch header");
    }
    WriteBatch batch;
    WriteBatchInternal::SetContents(&batch, record);
    LSMKV_TRY(WriteBatchInternal::InsertInto(&batch, mem.get()));
    ++records;

    const SequenceNumber last =
        WriteBatchInternal::Sequence(&batch) + WriteBatchInternal::Count(&batch) - 1;
    if (last > *max_sequence) *max_sequence = last;

    // Flush as we go, so recovering a very large log does not need all of it
    // in memory at once.
    if (mem->ApproximateMemoryUsage() > options_.write_buffer_size) {
      LSMKV_TRY(WriteLevel0Table(mem.get(), edit));
      mem = std::make_shared<MemTable>();
    }
  }
  LSMKV_TRY(reporter.first());

  if (mem->EntryCount() > 0) LSMKV_TRY(WriteLevel0Table(mem.get(), edit));
  (void)records;
  return Status::OK();
}

Status DBImpl::WriteLevel0Table(MemTable* mem, VersionEdit* edit) {
  FileMetaData meta;
  meta.number = versions_->NewFileNumber();
  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_outputs_.insert(meta.number);
  }

  const std::string path = TableFileName(dbname_, meta.number);
  Status s;
  {
    std::unique_ptr<WritableFile> file;
    s = env_->NewWritableFile(path, &file);
    if (!s.ok()) return s;

    TableBuilder builder(options_, file.get());
    auto iter = mem->NewIterator();
    iter->SeekToFirst();
    if (iter->Valid()) {
      meta.smallest.assign(iter->key());
      for (; iter->Valid(); iter->Next()) {
        meta.largest.assign(iter->key());
        builder.Add(iter->key(), iter->value());
      }
      s = builder.Finish();
      if (s.ok()) meta.file_size = builder.FileSize();
    } else {
      builder.Abandon();
    }
    if (s.ok()) s = file->Sync();
    if (s.ok()) s = file->Close();
    else file->Close();
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    pending_outputs_.erase(meta.number);
    counters_.bytes_written += meta.file_size;
    ++counters_.flushes;
  }

  if (!s.ok() || meta.file_size == 0) {
    env_->RemoveFile(path);
    versions_->ReuseFileNumber(meta.number);
    return s;
  }
  // A flushed memtable always goes to level 0, where files may overlap.
  edit->AddFile(0, meta);
  return Status::OK();
}

Status DBImpl::CompactMemTable() {
  std::shared_ptr<MemTable> frozen;
  {
    std::lock_guard<std::mutex> lock(mu_);
    frozen = imm_;
  }
  if (!frozen) return Status::OK();

  VersionEdit edit;
  LSMKV_TRY(WriteLevel0Table(frozen.get(), &edit));

  // The log that fed this memtable is no longer needed: its contents are in
  // a table now.
  edit.SetLogNumber(logfile_number_.load(std::memory_order_acquire));
  LSMKV_TRY(versions_->LogAndApply(&edit));

  {
    std::lock_guard<std::mutex> lock(mu_);
    imm_.reset();
    has_imm_.store(false, std::memory_order_release);
  }
  RemoveObsoleteFiles();
  writers_can_proceed_.notify_all();
  return Status::OK();
}

SequenceNumber DBImpl::OldestSnapshot() const {
  if (snapshots_.empty()) return versions_->LastSequence();
  return snapshots_.begin()->first;
}

Status DBImpl::DoCompaction(Compaction* c) {
  if (c->IsTrivialMove()) {
    // Nothing to merge with, so the file can simply be re-labelled as
    // belonging to the next level. No bytes are rewritten.
    VersionEdit edit;
    edit.RemoveFile(c->level, c->inputs[0][0].number);
    edit.AddFile(c->level + 1, c->inputs[0][0]);
    LSMKV_TRY(versions_->LogAndApply(&edit));
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.trivial_moves;
    return Status::OK();
  }

  SequenceNumber oldest_snapshot;
  {
    std::lock_guard<std::mutex> lock(mu_);
    oldest_snapshot = OldestSnapshot();
  }

  // One merged stream over every input file, newest first.
  std::vector<std::unique_ptr<Iterator>> inputs;
  ReadOptions read_options;
  read_options.verify_checksums = options_.paranoid_checks;
  read_options.fill_cache = false;  // a compaction scan should not evict the working set
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs[which]) {
      inputs.push_back(table_cache_->NewIterator(read_options, f.number, f.file_size));
    }
  }
  auto iter = NewMergingIterator(std::move(inputs), CompareInternal);

  VersionEdit edit;
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs[which]) edit.RemoveFile(c->level + which, f.number);
  }

  std::unique_ptr<WritableFile> file;
  std::unique_ptr<TableBuilder> builder;
  FileMetaData meta;
  uint64_t dropped = 0;
  uint64_t bytes = 0;

  std::string current_user_key;
  bool has_current_user_key = false;
  SequenceNumber last_sequence_for_key = kMaxSequenceNumber;

  auto finish_output = [&]() -> Status {
    if (!builder) return Status::OK();
    LSMKV_TRY(builder->Finish());
    meta.file_size = builder->FileSize();
    bytes += meta.file_size;
    LSMKV_TRY(file->Sync());
    LSMKV_TRY(file->Close());
    edit.AddFile(c->level + 1, meta);
    {
      std::lock_guard<std::mutex> lock(mu_);
      pending_outputs_.erase(meta.number);
    }
    builder.reset();
    file.reset();
    return Status::OK();
  };

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey parsed;
    if (!ParseInternalKey(iter->key(), &parsed)) {
      return Status::Corruption("malformed key during compaction");
    }

    if (!has_current_user_key || parsed.user_key != std::string_view(current_user_key)) {
      current_user_key.assign(parsed.user_key);
      has_current_user_key = true;
      last_sequence_for_key = kMaxSequenceNumber;
    }

    bool drop = false;
    if (last_sequence_for_key <= oldest_snapshot) {
      // An older version of a key no live snapshot can reach.
      drop = true;
    } else if (parsed.type == ValueType::kDeletion && parsed.sequence <= oldest_snapshot &&
               c->bottommost) {
      // A tombstone can only go once nothing deeper could still hold the
      // value it is hiding.
      drop = true;
    }
    last_sequence_for_key = parsed.sequence;

    if (drop) {
      ++dropped;
      continue;
    }

    if (!builder) {
      meta = FileMetaData{};
      meta.number = versions_->NewFileNumber();
      {
        std::lock_guard<std::mutex> lock(mu_);
        pending_outputs_.insert(meta.number);
      }
      LSMKV_TRY(env_->NewWritableFile(TableFileName(dbname_, meta.number), &file));
      builder = std::make_unique<TableBuilder>(options_, file.get());
      meta.smallest.assign(iter->key());
    }

    meta.largest.assign(iter->key());
    builder->Add(iter->key(), iter->value());

    if (builder->FileSize() >= options_.max_file_size) LSMKV_TRY(finish_output());
  }
  LSMKV_TRY(iter->status());
  LSMKV_TRY(finish_output());

  LSMKV_TRY(versions_->LogAndApply(&edit));

  {
    std::lock_guard<std::mutex> lock(mu_);
    ++counters_.compactions;
    counters_.keys_dropped += dropped;
    counters_.bytes_compacted += bytes;
  }
  RemoveObsoleteFiles();
  return Status::OK();
}

void DBImpl::RemoveObsoleteFiles() {
  std::set<uint64_t> live;
  versions_->AddLiveFiles(&live);
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (const uint64_t n : pending_outputs_) live.insert(n);
  }

  std::vector<std::string> children;
  if (!env_->GetChildren(dbname_, &children).ok()) return;

  for (const auto& name : children) {
    if (name == "CURRENT" || name == "LOCK") continue;

    const bool is_manifest = name.rfind("MANIFEST-", 0) == 0;
    const uint64_t number =
        is_manifest ? std::strtoull(name.c_str() + 9, nullptr, 10)
                    : std::strtoull(name.c_str(), nullptr, 10);

    bool keep = true;
    if (is_manifest) {
      keep = number >= versions_->ManifestFileNumber();
    } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0) {
      // The current log and any log the manifest still points at.
      keep = number >= versions_->LogNumber() ||
             number == logfile_number_.load(std::memory_order_acquire);
    } else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tbl") == 0) {
      keep = live.count(number) > 0;
    } else {
      keep = true;  // not ours; leave it alone
    }

    if (!keep) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tbl") == 0) {
        table_cache_->Evict(number);
      }
      env_->RemoveFile(dbname_ + "/" + name);
    }
  }
}

void DBImpl::MaybeScheduleCompaction() {
  if (shutting_down_ || background_running_) return;
  background_work_pending_ = true;
  background_done_.notify_all();
}

void DBImpl::BackgroundLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    background_done_.wait(lock, [this] { return shutting_down_ || background_work_pending_; });
    if (shutting_down_) break;

    background_work_pending_ = false;
    background_running_ = true;
    const bool flush_needed = imm_ != nullptr;

    lock.unlock();
    Status s;
    if (flush_needed) {
      s = CompactMemTable();
    } else if (auto c = versions_->PickCompaction()) {
      s = DoCompaction(c.get());
    }
    lock.lock();

    background_running_ = false;
    if (!s.ok() && background_error_.ok()) background_error_ = s;

    // Keep going while there is more to do, but stop retrying after an error
    // rather than spinning on it.
    if (background_error_.ok() && (imm_ != nullptr || versions_->PickCompaction() != nullptr)) {
      background_work_pending_ = true;
    }
    writers_can_proceed_.notify_all();
    background_done_.notify_all();
  }
  background_running_ = false;
  background_done_.notify_all();
}

void DBImpl::WaitForCompaction() {
  std::unique_lock<std::mutex> lock(mu_);
  background_done_.wait(lock, [this] {
    return shutting_down_ || (!background_running_ && !background_work_pending_ && imm_ == nullptr);
  });
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  if (updates == nullptr || updates->Count() == 0) return Status::OK();

  std::unique_lock<std::mutex> lock(mu_);
  if (!background_error_.ok()) return background_error_;

  // Freeze a full memtable, and hold the writer back if level 0 has grown
  // faster than compaction can drain it.
  while (true) {
    if (!background_error_.ok()) return background_error_;

    const size_t level0 = versions_->current()->LevelFiles(0).size();
    if (level0 >= static_cast<size_t>(options_.level0_stop_trigger)) {
      ++counters_.write_stalls;
      MaybeScheduleCompaction();
      writers_can_proceed_.wait_for(lock, std::chrono::milliseconds(50));
      continue;
    }
    if (mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) break;
    if (imm_ != nullptr) {
      // A previous memtable is still being flushed; wait for it rather than
      // dropping a second one on the background thread.
      MaybeScheduleCompaction();
      writers_can_proceed_.wait_for(lock, std::chrono::milliseconds(50));
      continue;
    }

    // Roll over to a new log and a new memtable. The old one becomes
    // immutable and is handed to the background thread.
    const uint64_t new_log_number = versions_->NewFileNumber();
    std::unique_ptr<WritableFile> new_log_file;
    const Status s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &new_log_file);
    if (!s.ok()) {
      versions_->ReuseFileNumber(new_log_number);
      return s;
    }
    if (logfile_) logfile_->Close();
    logfile_ = std::move(new_log_file);
    logfile_number_.store(new_log_number, std::memory_order_release);
    log_ = std::make_unique<log::Writer>(logfile_.get());
    imm_ = mem_;
    has_imm_.store(true, std::memory_order_release);
    mem_ = std::make_shared<MemTable>();
    MaybeScheduleCompaction();
  }

  const SequenceNumber sequence = versions_->LastSequence() + 1;
  WriteBatchInternal::SetSequence(updates, sequence);
  const uint32_t count = updates->Count();

  // The log is written before the memtable, so a crash can lose a write that
  // was never acknowledged but never shows one that was.
  Status s = log_->AddRecord(WriteBatchInternal::Contents(updates));
  if (s.ok() && options.sync) s = logfile_->Sync();
  if (!s.ok()) {
    background_error_ = s;
    return s;
  }

  s = WriteBatchInternal::InsertInto(updates, mem_.get());
  if (!s.ok()) {
    background_error_ = s;
    return s;
  }

  versions_->SetLastSequence(sequence + count - 1);
  counters_.writes += count;
  counters_.bytes_written += updates->ApproximateSize();
  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, std::string_view key, std::string_view value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, std::string_view key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status DBImpl::Get(const ReadOptions& options, std::string_view key, std::string* value) {
  SequenceNumber sequence;
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<const Version> version;

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!background_error_.ok()) return background_error_;
    sequence = options.snapshot != nullptr
                   ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence
                   : versions_->LastSequence();
    mem = mem_;
    imm = imm_;
    version = versions_->current();
    ++counters_.reads;
  }

  // Everything below runs without the lock. The memtables are shared_ptr and
  // the version is immutable, so a concurrent flush or compaction cannot
  // pull any of it out from under this read.
  const LookupKey lookup(key, sequence);
  Status s;
  if (mem->Get(lookup, value, &s)) return s;
  if (imm && imm->Get(lookup, value, &s)) return s;
  return version->Get(options, lookup, value, table_cache_.get());
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  SequenceNumber sequence;
  std::vector<std::unique_ptr<Iterator>> parts;

  {
    std::lock_guard<std::mutex> lock(mu_);
    sequence = options.snapshot != nullptr
                   ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence
                   : versions_->LastSequence();

    // Newest source first, so the merging iterator prefers newer versions.
    // Each part holds a shared_ptr to what it reads, so the iterator stays
    // valid across a flush.
    auto hold = [](std::shared_ptr<MemTable> table) {
      class Held final : public Iterator {
       public:
        Held(std::shared_ptr<MemTable> t, std::unique_ptr<Iterator> i)
            : table_(std::move(t)), iter_(std::move(i)) {}
        bool Valid() const override { return iter_->Valid(); }
        void SeekToFirst() override { iter_->SeekToFirst(); }
        void SeekToLast() override { iter_->SeekToLast(); }
        void Seek(std::string_view t) override { iter_->Seek(t); }
        void Next() override { iter_->Next(); }
        void Prev() override { iter_->Prev(); }
        std::string_view key() const override { return iter_->key(); }
        std::string_view value() const override { return iter_->value(); }
        Status status() const override { return iter_->status(); }

       private:
        std::shared_ptr<MemTable> table_;
        std::unique_ptr<Iterator> iter_;
      };
      auto inner = table->NewIterator();
      return std::unique_ptr<Iterator>(new Held(std::move(table), std::move(inner)));
    };

    parts.push_back(hold(mem_));
    if (imm_) parts.push_back(hold(imm_));
    versions_->current()->AddIterators(options, table_cache_.get(), &parts);
  }

  auto merged = NewMergingIterator(std::move(parts), CompareInternal);
  return std::make_unique<DBIter>(std::move(merged), sequence);
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> lock(mu_);
  const SequenceNumber sequence = versions_->LastSequence();
  ++snapshots_[sequence];
  snapshot_handles_.push_back(std::make_unique<SnapshotImpl>(sequence));
  return snapshot_handles_.back().get();
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return;
  std::lock_guard<std::mutex> lock(mu_);
  const SequenceNumber sequence = static_cast<const SnapshotImpl*>(snapshot)->sequence;
  const auto it = snapshots_.find(sequence);
  if (it != snapshots_.end() && --it->second <= 0) snapshots_.erase(it);
  for (auto h = snapshot_handles_.begin(); h != snapshot_handles_.end(); ++h) {
    if (h->get() == snapshot) {
      snapshot_handles_.erase(h);
      break;
    }
  }
}

Status DBImpl::CompactRange(std::string_view begin, std::string_view end) {
  // Flush whatever is in memory first, so the range compaction sees it.
  {
    std::unique_lock<std::mutex> lock(mu_);
    if (mem_->EntryCount() > 0 && imm_ == nullptr) {
      const uint64_t new_log_number = versions_->NewFileNumber();
      std::unique_ptr<WritableFile> new_log_file;
      LSMKV_TRY(env_->NewWritableFile(LogFileName(dbname_, new_log_number), &new_log_file));
      if (logfile_) logfile_->Close();
      logfile_ = std::move(new_log_file);
      logfile_number_.store(new_log_number, std::memory_order_release);
      log_ = std::make_unique<log::Writer>(logfile_.get());
      imm_ = mem_;
      has_imm_.store(true, std::memory_order_release);
      mem_ = std::make_shared<MemTable>();
      MaybeScheduleCompaction();
    }
  }
  WaitForCompaction();

  // Then compact the range down, level by level, until nothing overlaps.
  for (int pass = 0; pass < kNumLevels; ++pass) {
    auto c = versions_->CompactRange(begin, end);
    if (!c) break;
    LSMKV_TRY(DoCompaction(c.get()));
  }

  std::lock_guard<std::mutex> lock(mu_);
  return background_error_;
}

std::string DBImpl::Stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::string out = "lsmkv " + dbname_ + "\n";
  out += versions_->LevelSummary();

  char buf[640];
  std::snprintf(buf, sizeof(buf),
                "  writes        %10" PRIu64 "\n"
                "  reads         %10" PRIu64 "\n"
                "  flushes       %10" PRIu64 "\n"
                "  compactions   %10" PRIu64 "\n"
                "  trivial moves %10" PRIu64 "\n"
                "  keys dropped  %10" PRIu64 "\n"
                "  write stalls  %10" PRIu64 "\n"
                "  bytes written %10" PRIu64 "\n"
                "  bytes merged  %10" PRIu64 "\n",
                counters_.writes, counters_.reads, counters_.flushes, counters_.compactions,
                counters_.trivial_moves, counters_.keys_dropped, counters_.write_stalls,
                counters_.bytes_written, counters_.bytes_compacted);
  out += buf;
  return out;
}

Status DB::Open(const Options& options, const std::string& name, std::unique_ptr<DB>* dbptr) {
  auto db = std::make_unique<DBImpl>(options, name);
  LSMKV_TRY(db->Open());
  *dbptr = std::move(db);
  return Status::OK();
}

Status DestroyDB(const std::string& name, const Options& options) {
  Env* env = options.env != nullptr ? options.env : Env::Default();
  std::vector<std::string> children;
  const Status list = env->GetChildren(name, &children);
  if (!list.ok()) return Status::OK();  // nothing there to destroy

  Status result;
  for (const auto& child : children) {
    const Status s = env->RemoveFile(name + "/" + child);
    if (!s.ok() && result.ok()) result = s;
  }
  env->RemoveDir(name);
  return result;
}

}  // namespace lsmkv
