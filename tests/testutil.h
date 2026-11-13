#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unistd.h>
// In-memory files and a fault-injecting Env, shared by the log, table and
// recovery tests.
#pragma once

#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "util/env.h"

namespace lsmkv::test {

// A writable file that appends to a std::string, so a test can inspect the
// exact bytes a writer produced.
class StringSink final : public WritableFile {
 public:
  explicit StringSink(std::string* contents) : contents_(contents) {}

  Status Append(std::string_view data) override {
    contents_->append(data);
    return Status::OK();
  }
  Status Flush() override { return Status::OK(); }
  Status Sync() override {
    ++syncs_;
    return Status::OK();
  }
  Status Close() override { return Status::OK(); }

  int syncs() const { return syncs_; }

 private:
  std::string* contents_;
  int syncs_ = 0;
};

// Sequential reads over a string. `Read` returns short reads at the end, the
// way a real file does.
class StringSource final : public SequentialFile {
 public:
  explicit StringSource(std::string_view contents) : contents_(contents) {}

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    const size_t available = contents_.size() - pos_;
    const size_t to_read = n < available ? n : available;
    std::memcpy(scratch, contents_.data() + pos_, to_read);
    pos_ += to_read;
    *result = std::string_view(scratch, to_read);
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    pos_ += static_cast<size_t>(n);
    if (pos_ > contents_.size()) pos_ = contents_.size();
    return Status::OK();
  }

 private:
  std::string_view contents_;
  size_t pos_ = 0;
};

class StringRandomSource final : public RandomAccessFile {
 public:
  explicit StringRandomSource(std::string contents) : contents_(std::move(contents)) {}

  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    if (offset > contents_.size()) return Status::IOError("read past end of file");
    const size_t available = contents_.size() - static_cast<size_t>(offset);
    const size_t to_read = n < available ? n : available;
    std::memcpy(scratch, contents_.data() + offset, to_read);
    *result = std::string_view(scratch, to_read);
    return Status::OK();
  }

 private:
  const std::string contents_;
};

// A temporary directory removed on destruction, so a failing test does not
// leave files behind for the next one to trip over.
class ScopedTempDir {
 public:
  explicit ScopedTempDir(std::string_view label) {
    static std::atomic<int> counter{0};
    const char* base = std::getenv("TMPDIR");
    path_ = std::string(base ? base : "/tmp") + "/lsmkv_" + std::string(label) + "_" +
            std::to_string(::getpid()) + "_" + std::to_string(counter.fetch_add(1));
    Remove();
    Env::Default()->CreateDir(path_);
  }
  ~ScopedTempDir() { Remove(); }

  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::string& path() const { return path_; }

 private:
  void Remove() {
    Env* env = Env::Default();
    std::vector<std::string> children;
    if (env->GetChildren(path_, &children).ok()) {
      for (const auto& child : children) env->RemoveFile(path_ + "/" + child);
    }
    env->RemoveDir(path_);
  }

  std::string path_;
};

// Simulates a machine crash. Every write to every file is recorded; on
// "crash", the files are rewritten keeping only the bytes that had been
// flushed and synced, and anything written after the crash point is dropped.
//
// This is what lets the recovery tests assert the real durability contract:
// a write that returned after a sync must still be there, and one that was
// never synced may be gone, but the database must still open.
class CrashEnv final : public EnvWrapper {
 public:
  explicit CrashEnv(Env* target) : EnvWrapper(target) {}

  Status NewWritableFile(const std::string& path,
                         std::unique_ptr<WritableFile>* result) override {
    std::unique_ptr<WritableFile> real;
    LSMKV_TRY(target()->NewWritableFile(path, &real));
    *result = std::make_unique<CrashFile>(this, path, std::move(real));
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& path,
                           std::unique_ptr<WritableFile>* result) override {
    std::unique_ptr<WritableFile> real;
    LSMKV_TRY(target()->NewAppendableFile(path, &real));
    *result = std::make_unique<CrashFile>(this, path, std::move(real));
    return Status::OK();
  }

  // After this many bytes have been appended across all files, every further
  // write is silently dropped, as if the machine had stopped.
  void CrashAfterBytes(uint64_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    limit_ = n;
    written_ = 0;
    crashed_ = false;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mu_);
    limit_ = UINT64_MAX;
    written_ = 0;
    crashed_ = false;
  }

  bool crashed() const {
    std::lock_guard<std::mutex> lock(mu_);
    return crashed_;
  }

  uint64_t bytes_written() const {
    std::lock_guard<std::mutex> lock(mu_);
    return written_;
  }

  // Returns how many of `n` bytes may be written before the crash point.
  size_t Allow(size_t n) {
    std::lock_guard<std::mutex> lock(mu_);
    if (written_ >= limit_) {
      crashed_ = true;
      return 0;
    }
    const uint64_t room = limit_ - written_;
    const size_t allowed = room < n ? static_cast<size_t>(room) : n;
    written_ += allowed;
    if (allowed < n) crashed_ = true;
    return allowed;
  }

 private:
  class CrashFile final : public WritableFile {
   public:
    CrashFile(CrashEnv* env, std::string path, std::unique_ptr<WritableFile> real)
        : env_(env), path_(std::move(path)), real_(std::move(real)) {}

    Status Append(std::string_view data) override {
      const size_t allowed = env_->Allow(data.size());
      if (allowed == 0) return Status::OK();  // a crashed machine reports nothing
      return real_->Append(data.substr(0, allowed));
    }
    Status Flush() override { return real_->Flush(); }
    Status Sync() override { return real_->Sync(); }
    Status Close() override { return real_->Close(); }

   private:
    CrashEnv* env_;
    const std::string path_;
    std::unique_ptr<WritableFile> real_;
  };

  mutable std::mutex mu_;
  uint64_t limit_ = UINT64_MAX;
  uint64_t written_ = 0;
  bool crashed_ = false;
};

// Deterministic random strings, so a failure can be reproduced from the seed
// printed by the test.
inline std::string RandomString(std::mt19937& rng, size_t len) {
  std::string out(len, '\0');
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<char>(' ' + rng() % 95);
  return out;
}

// A key space with shared prefixes, which is what exercises the prefix
// compression in the block format.
inline std::string NumericKey(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012llu", static_cast<unsigned long long>(i));
  return buf;
}

}  // namespace lsmkv::test
