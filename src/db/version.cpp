#include "db/version.h"

#include <algorithm>
#include <cstdio>

#include "table/block.h"
#include "table/iterators.h"
#include "util/coding.h"

namespace lsmkv {
namespace {

// Tags in the manifest encoding. New tags can be added without breaking an
// older reader's ability to skip what it does not know, because every record
// is length-delimited by the log format.
enum Tag : uint32_t {
  kTagLogNumber = 1,
  kTagNextFileNumber = 2,
  kTagLastSequence = 3,
  kTagCompactPointer = 4,
  kTagDeletedFile = 5,
  kTagNewFile = 6,
};

// Level sizing: level 1 holds 10 MB, and each level below is ten times
// larger. Keeping the ratio fixed bounds total write amplification at about
// 10 per level.
double MaxBytesForLevel(int level) {
  double result = 10.0 * 1048576.0;
  while (level > 1) {
    result *= 10;
    --level;
  }
  return result;
}

bool NewestFirst(const FileMetaData& a, const FileMetaData& b) { return a.number > b.number; }

}  // namespace

void VersionEdit::Clear() {
  log_number_ = 0;
  next_file_number_ = 0;
  last_sequence_ = 0;
  has_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  compact_pointers_.clear();
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_log_number_) {
    PutVarint32(dst, kTagLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kTagNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kTagLastSequence);
    PutVarint64(dst, last_sequence_);
  }
  for (const auto& [level, key] : compact_pointers_) {
    PutVarint32(dst, kTagCompactPointer);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutLengthPrefixed(dst, key);
  }
  for (const auto& [level, number] : deleted_files_) {
    PutVarint32(dst, kTagDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, file] : new_files_) {
    PutVarint32(dst, kTagNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, file.number);
    PutVarint64(dst, file.file_size);
    PutLengthPrefixed(dst, file.smallest);
    PutLengthPrefixed(dst, file.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  Clear();
  std::string_view input = src;
  uint32_t tag = 0;

  auto bad = [](const char* what) { return Status::Corruption(std::string("VersionEdit: ") + what); };

  while (GetVarint32(&input, &tag)) {
    switch (tag) {
      case kTagLogNumber:
        if (!GetVarint64(&input, &log_number_)) return bad("log number");
        has_log_number_ = true;
        break;
      case kTagNextFileNumber:
        if (!GetVarint64(&input, &next_file_number_)) return bad("next file number");
        has_next_file_number_ = true;
        break;
      case kTagLastSequence:
        if (!GetVarint64(&input, &last_sequence_)) return bad("last sequence");
        has_last_sequence_ = true;
        break;
      case kTagCompactPointer: {
        uint32_t level = 0;
        std::string_view key;
        if (!GetVarint32(&input, &level) || !GetLengthPrefixed(&input, &key)) {
          return bad("compact pointer");
        }
        if (level >= kNumLevels) return bad("compact pointer level out of range");
        compact_pointers_.emplace_back(static_cast<int>(level), std::string(key));
        break;
      }
      case kTagDeletedFile: {
        uint32_t level = 0;
        uint64_t number = 0;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &number)) {
          return bad("deleted file");
        }
        if (level >= kNumLevels) return bad("deleted file level out of range");
        deleted_files_.emplace(static_cast<int>(level), number);
        break;
      }
      case kTagNewFile: {
        uint32_t level = 0;
        FileMetaData file;
        std::string_view smallest, largest;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &file.number) ||
            !GetVarint64(&input, &file.file_size) || !GetLengthPrefixed(&input, &smallest) ||
            !GetLengthPrefixed(&input, &largest)) {
          return bad("new file");
        }
        if (level >= kNumLevels) return bad("new file level out of range");
        // An internal key shorter than its tag would break every comparison
        // made against it, so reject it at the door.
        if (smallest.size() < 8 || largest.size() < 8) return bad("new file has a malformed key");
        file.smallest.assign(smallest);
        file.largest.assign(largest);
        new_files_.emplace_back(static_cast<int>(level), file);
        break;
      }
      default:
        return bad("unknown tag");
    }
  }
  if (!input.empty()) return bad("trailing bytes");
  return Status::OK();
}

uint64_t Version::LevelBytes(int level) const {
  uint64_t sum = 0;
  for (const auto& f : files_[level]) sum += f.file_size;
  return sum;
}

double Version::CompactionScore(int* level, const Options& options) const {
  double best = -1;
  int best_level = -1;
  for (int i = 0; i < kNumLevels - 1; ++i) {
    const double score =
        i == 0 ? static_cast<double>(files_[0].size()) /
                     static_cast<double>(options.level0_compaction_trigger)
               : static_cast<double>(LevelBytes(i)) / MaxBytesForLevel(i);
    if (score > best) {
      best = score;
      best_level = i;
    }
  }
  *level = best_level;
  return best;
}

std::string Version::DebugSummary() const {
  std::string out;
  for (int i = 0; i < kNumLevels; ++i) {
    if (files_[i].empty()) continue;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "  level %d: %3zu files  %8.2f MB\n", i, files_[i].size(),
                  static_cast<double>(LevelBytes(i)) / 1048576.0);
    out += buf;
  }
  if (out.empty()) out = "  (no files)\n";
  return out;
}

Status Version::Get(const ReadOptions& options, const LookupKey& key, std::string* value,
                    TableCache* cache) const {
  const std::string_view user_key = key.user_key();
  const std::string_view internal_key = key.internal_key();

  for (int level = 0; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;

    // Level 0 files overlap, so every one whose range covers the key has to
    // be read, newest first. Below that the files are disjoint, so a binary
    // search finds the single candidate.
    std::vector<const FileMetaData*> candidates;
    if (level == 0) {
      for (const auto& f : files_[0]) {
        if (user_key >= ExtractUserKey(f.smallest) && user_key <= ExtractUserKey(f.largest)) {
          candidates.push_back(&f);
        }
      }
      std::sort(candidates.begin(), candidates.end(),
                [](const FileMetaData* a, const FileMetaData* b) { return NewestFirst(*a, *b); });
    } else {
      const auto it = std::lower_bound(
          files_[level].begin(), files_[level].end(), internal_key,
          [](const FileMetaData& f, std::string_view target) {
            return CompareInternalKey(f.largest, target) < 0;
          });
      if (it != files_[level].end() && user_key >= ExtractUserKey(it->smallest)) {
        candidates.push_back(&*it);
      }
    }

    for (const FileMetaData* f : candidates) {
      bool found = false;
      Status found_status;
      const Status s = cache->Get(options, f->number, f->file_size, internal_key,
                                  [&](std::string_view k, std::string_view v) {
                                    ParsedInternalKey parsed;
                                    if (!ParseInternalKey(k, &parsed)) {
                                      found_status = Status::Corruption("bad key in table");
                                      found = true;
                                      return;
                                    }
                                    if (parsed.user_key != user_key) return;  // key is absent here
                                    found = true;
                                    if (parsed.type == ValueType::kValue) {
                                      value->assign(v);
                                      found_status = Status::OK();
                                    } else {
                                      // A tombstone is the answer: stop,
                                      // rather than finding an older value
                                      // in a deeper level.
                                      found_status = Status::NotFound("");
                                    }
                                  });
      if (!s.ok()) return s;
      if (found) return found_status;
    }
  }
  return Status::NotFound("");
}

void Version::AddIterators(const ReadOptions& options, TableCache* cache,
                           std::vector<std::unique_ptr<Iterator>>* out) const {
  // Level 0, newest file first, so a merging iterator prefers newer versions.
  std::vector<const FileMetaData*> level0;
  for (const auto& f : files_[0]) level0.push_back(&f);
  std::sort(level0.begin(), level0.end(),
            [](const FileMetaData* a, const FileMetaData* b) { return NewestFirst(*a, *b); });
  for (const FileMetaData* f : level0) {
    out->push_back(cache->NewIterator(options, f->number, f->file_size));
  }

  // Deeper levels are disjoint and sorted, so one two-level iterator per
  // level reads them as a single stream without holding every table open.
  for (int level = 1; level < kNumLevels; ++level) {
    if (files_[level].empty()) continue;

    // An index over the level: each entry maps the file's largest key to the
    // file number and size, which the second level opens.
    BlockBuilder index(1);
    for (const auto& f : files_[level]) {
      std::string handle;
      PutVarint64(&handle, f.number);
      PutVarint64(&handle, f.file_size);
      index.Add(f.largest, handle);
    }
    std::unique_ptr<Block> block;
    if (!Block::Create(std::string(index.Finish()), CompareInternal, &block).ok()) continue;

    // The block must outlive the iterator built over it.
    auto shared = std::shared_ptr<Block>(std::move(block));
    class IndexHolder final : public Iterator {
     public:
      IndexHolder(std::shared_ptr<Block> b, std::unique_ptr<Iterator> i)
          : block_(std::move(b)), iter_(std::move(i)) {}
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
      std::shared_ptr<Block> block_;
      std::unique_ptr<Iterator> iter_;
    };
    auto index_iter = std::make_unique<IndexHolder>(shared, shared->NewIterator());

    out->push_back(NewTwoLevelIterator(
        std::move(index_iter), [cache, options](std::string_view handle) {
          std::string_view input = handle;
          uint64_t number = 0, size = 0;
          if (!GetVarint64(&input, &number) || !GetVarint64(&input, &size)) {
            return NewErrorIterator(Status::Corruption("bad file handle in level index"));
          }
          return cache->NewIterator(options, number, size);
        }));
  }
}

std::vector<FileMetaData> FilesOverlapping(const std::vector<FileMetaData>& files,
                                           std::string_view begin, std::string_view end,
                                           bool level_zero) {
  std::vector<FileMetaData> result;
  for (const auto& f : files) {
    const std::string_view smallest = ExtractUserKey(f.smallest);
    const std::string_view largest = ExtractUserKey(f.largest);
    if (!begin.empty() && largest < begin) continue;
    if (!end.empty() && smallest > end) continue;
    result.push_back(f);
  }
  (void)level_zero;
  return result;
}

VersionSet::VersionSet(std::string dbname, const Options& options, Env* env)
    : dbname_(std::move(dbname)),
      options_(options),
      env_(env),
      current_(std::make_shared<Version>()) {}

VersionSet::~VersionSet() = default;

void VersionSet::Apply(const VersionEdit& edit, const Version& base, Version* out) const {
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : base.files_[level]) {
      if (edit.deleted_files_.count({level, f.number}) == 0) out->files_[level].push_back(f);
    }
  }
  for (const auto& [level, f] : edit.new_files_) out->files_[level].push_back(f);
  Finalize(out);
}

void VersionSet::Finalize(Version* v) const {
  // Level 0 is ordered by file number so the newest is found first; the rest
  // by key, which is what makes the binary search in Get valid.
  std::sort(v->files_[0].begin(), v->files_[0].end(),
            [](const FileMetaData& a, const FileMetaData& b) { return a.number < b.number; });
  for (int level = 1; level < kNumLevels; ++level) {
    std::sort(v->files_[level].begin(), v->files_[level].end(),
              [](const FileMetaData& a, const FileMetaData& b) {
                return CompareInternalKey(a.smallest, b.smallest) < 0;
              });
  }
}

Status VersionSet::WriteSnapshot(log::Writer* writer) {
  // The manifest starts with the complete current state, so replaying it
  // never needs an earlier manifest. Called with write_mu_ held.
  VersionEdit edit;
  edit.SetNextFile(NextFileNumber());
  edit.SetLastSequence(LastSequence());
  edit.SetLogNumber(LogNumber());
  for (int level = 0; level < kNumLevels; ++level) {
    if (!compact_pointer_[level].empty()) edit.SetCompactPointer(level, compact_pointer_[level]);
    for (const auto& f : current_->files_[level]) edit.AddFile(level, f);
  }
  std::string record;
  edit.EncodeTo(&record);
  return writer->AddRecord(record);
}

Status VersionSet::CreateNew(uint64_t log_number) {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  manifest_file_number_.store(NewFileNumber(), std::memory_order_release);
  SetLogNumber(log_number);

  const std::string manifest = ManifestFileName(dbname_, ManifestFileNumber());
  LSMKV_TRY(env_->NewWritableFile(manifest, &manifest_file_));
  manifest_log_ = std::make_unique<log::Writer>(manifest_file_.get());

  Status s = WriteSnapshot(manifest_log_.get());
  if (s.ok()) s = manifest_file_->Sync();
  if (!s.ok()) {
    manifest_log_.reset();
    manifest_file_.reset();
    env_->RemoveFile(manifest);
    return s;
  }

  // CURRENT is written to a temporary and renamed, so a crash leaves either
  // the old name or the new one, never a half-written pointer.
  const std::string temp = TempFileName(dbname_, ManifestFileNumber());
  char buf[64];
  std::snprintf(buf, sizeof(buf), "MANIFEST-%06llu\n",
                static_cast<unsigned long long>(ManifestFileNumber()));
  s = WriteStringToFile(env_, buf, temp, true);
  if (s.ok()) s = env_->RenameFile(temp, CurrentFileName(dbname_));
  if (s.ok()) s = env_->SyncDir(dbname_);
  if (!s.ok()) env_->RemoveFile(temp);
  return s;
}

namespace {

class ManifestReporter final : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    if (first_.ok()) first_ = status;
    dropped_ += bytes;
  }
  const Status& first() const { return first_; }

 private:
  Status first_;
  size_t dropped_ = 0;
};

}  // namespace

Status VersionSet::Recover() {
  std::string current;
  LSMKV_TRY(ReadFileToString(env_, CurrentFileName(dbname_), &current));
  if (current.empty() || current.back() != '\n') {
    return Status::Corruption("CURRENT does not name a manifest");
  }
  current.pop_back();

  const std::string manifest = dbname_ + "/" + current;
  std::unique_ptr<SequentialFile> file;
  LSMKV_TRY(env_->NewSequentialFile(manifest, &file));

  auto version = std::make_shared<Version>();
  bool have_next_file = false, have_last_sequence = false, have_log_number = false;

  {
    ManifestReporter reporter;
    log::Reader reader(file.get(), &reporter, true, 0);
    std::string scratch;
    std::string_view record;
    while (reader.ReadRecord(&record, &scratch)) {
      VersionEdit edit;
      LSMKV_TRY(edit.DecodeFrom(record));

      // Apply onto the version built so far.
      auto next = std::make_shared<Version>();
      Apply(edit, *version, next.get());
      version = next;

      if (edit.has_next_file_number_) {
        next_file_number_.store(edit.next_file_number_, std::memory_order_relaxed);
        have_next_file = true;
      }
      if (edit.has_last_sequence_) {
        SetLastSequence(edit.last_sequence_);
        have_last_sequence = true;
      }
      if (edit.has_log_number_) {
        SetLogNumber(edit.log_number_);
        have_log_number = true;
      }
      for (const auto& [level, key] : edit.compact_pointers_) compact_pointer_[level] = key;
    }
    if (!reporter.first().ok()) return reporter.first();
  }

  if (!have_next_file) return Status::Corruption("manifest records no next file number");
  if (!have_last_sequence) return Status::Corruption("manifest records no last sequence");
  if (!have_log_number) return Status::Corruption("manifest records no log number");

  // The manifest itself and every file it names must be accounted for before
  // the next file number is handed out.
  const std::string number_text = current.substr(std::string("MANIFEST-").size());
  const uint64_t manifest_number = std::strtoull(number_text.c_str(), nullptr, 10);
  manifest_file_number_.store(manifest_number, std::memory_order_release);
  if (NextFileNumber() <= manifest_number) {
    next_file_number_.store(manifest_number + 1, std::memory_order_relaxed);
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    current_ = version;
  }

  // Continue appending to the existing manifest rather than rewriting it.
  uint64_t manifest_size = 0;
  LSMKV_TRY(env_->GetFileSize(manifest, &manifest_size));
  LSMKV_TRY(env_->NewAppendableFile(manifest, &manifest_file_));
  manifest_log_ = std::make_unique<log::Writer>(manifest_file_.get(), manifest_size);
  return Status::OK();
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  // One manifest writer at a time: two overlapping calls would interleave
  // their records and leave a log that replays into the wrong state.
  std::lock_guard<std::mutex> write_lock(write_mu_);

  if (!edit->has_log_number_) edit->SetLogNumber(LogNumber());
  edit->SetNextFile(NextFileNumber());
  edit->SetLastSequence(LastSequence());

  auto next = std::make_shared<Version>();
  {
    std::lock_guard<std::mutex> lock(mu_);
    Apply(*edit, *current_, next.get());
  }

  // The manifest is written and synced before the new version is installed.
  // A crash between the two leaves the change on disk but not in memory,
  // which recovery fixes; the reverse would lose it.
  std::string record;
  edit->EncodeTo(&record);
  LSMKV_TRY(manifest_log_->AddRecord(record));
  LSMKV_TRY(manifest_file_->Sync());

  for (const auto& [level, key] : edit->compact_pointers_) compact_pointer_[level] = key;

  std::lock_guard<std::mutex> lock(mu_);
  current_ = next;
  return Status::OK();
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) const {
  std::lock_guard<std::mutex> lock(mu_);
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : current_->files_[level]) live->insert(f.number);
  }
}

std::string VersionSet::LevelSummary() const {
  std::lock_guard<std::mutex> lock(mu_);
  return current_->DebugSummary();
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  std::lock_guard<std::mutex> write_lock(write_mu_);
  std::shared_ptr<const Version> version;
  {
    std::lock_guard<std::mutex> lock(mu_);
    version = current_;
  }

  int level = 0;
  const double score = version->CompactionScore(&level, options_);
  if (score < 1.0 || level < 0) return nullptr;

  auto c = std::make_unique<Compaction>();
  c->level = level;

  if (level == 0) {
    // Level-0 files overlap each other, so a compaction has to take every
    // file that overlaps the range it starts with, transitively.
    c->inputs[0] = version->LevelFiles(0);
  } else {
    // Start after where the last compaction of this level finished, so the
    // whole key space gets attention in turn.
    const auto& files = version->LevelFiles(level);
    for (const auto& f : files) {
      if (compact_pointer_[level].empty() ||
          CompareInternalKey(f.largest, compact_pointer_[level]) > 0) {
        c->inputs[0].push_back(f);
        break;
      }
    }
    if (c->inputs[0].empty() && !files.empty()) c->inputs[0].push_back(files.front());
  }

  if (c->inputs[0].empty()) return nullptr;

  // Everything in the next level that overlaps the chosen range.
  std::string smallest = c->inputs[0].front().smallest;
  std::string largest = c->inputs[0].front().largest;
  for (const auto& f : c->inputs[0]) {
    if (CompareInternalKey(f.smallest, smallest) < 0) smallest = f.smallest;
    if (CompareInternalKey(f.largest, largest) > 0) largest = f.largest;
  }
  c->inputs[1] = FilesOverlapping(version->LevelFiles(level + 1), ExtractUserKey(smallest),
                                  ExtractUserKey(largest), false);

  // A tombstone may only be dropped where no deeper level could still hold
  // an older value for the key.
  c->bottommost = true;
  for (int deeper = level + 2; deeper < kNumLevels; ++deeper) {
    if (!version->LevelFiles(deeper).empty()) c->bottommost = false;
  }

  compact_pointer_[level] = largest;
  return c;
}

std::unique_ptr<Compaction> VersionSet::CompactRange(std::string_view begin, std::string_view end) {
  std::shared_ptr<const Version> version;
  {
    std::lock_guard<std::mutex> lock(mu_);
    version = current_;
  }

  for (int level = 0; level + 1 < kNumLevels; ++level) {
    auto overlapping = FilesOverlapping(version->LevelFiles(level), begin, end, level == 0);
    if (overlapping.empty()) continue;

    auto c = std::make_unique<Compaction>();
    c->level = level;
    c->inputs[0] = std::move(overlapping);

    std::string smallest = c->inputs[0].front().smallest;
    std::string largest = c->inputs[0].front().largest;
    for (const auto& f : c->inputs[0]) {
      if (CompareInternalKey(f.smallest, smallest) < 0) smallest = f.smallest;
      if (CompareInternalKey(f.largest, largest) > 0) largest = f.largest;
    }
    c->inputs[1] = FilesOverlapping(version->LevelFiles(level + 1), ExtractUserKey(smallest),
                                    ExtractUserKey(largest), false);
    c->bottommost = true;
    for (int deeper = level + 2; deeper < kNumLevels; ++deeper) {
      if (!version->LevelFiles(deeper).empty()) c->bottommost = false;
    }
    return c;
  }
  return nullptr;
}

}  // namespace lsmkv
