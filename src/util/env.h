// Every file operation the database performs goes through Env. The POSIX
// implementation is the only one shipped, but the indirection is what makes
// the crash tests possible: they install an Env that drops or truncates
// writes at a chosen point and then reopen the database.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/status.h"

namespace lsmkv {

class SequentialFile {
 public:
  SequentialFile() = default;
  virtual ~SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  // Reads up to n bytes. `scratch` must have room for n bytes; `result` may
  // point into it. A short read means end of file.
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  virtual ~RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;

  // Safe to call concurrently from several threads.
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result,
                      char* scratch) const = 0;
};

class WritableFile {
 public:
  WritableFile() = default;
  virtual ~WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  virtual Status Append(std::string_view data) = 0;
  // Pushes buffered bytes to the operating system. Survives a process crash,
  // not a machine crash.
  virtual Status Flush() = 0;
  // Pushes to stable storage. Survives a machine crash.
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

// Held for the lifetime of an open database so a second process cannot open
// the same directory.
class FileLock {
 public:
  FileLock() = default;
  virtual ~FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
};

class Env {
 public:
  Env() = default;
  virtual ~Env() = default;
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Process-wide POSIX environment. Never freed.
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& path,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(const std::string& path,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;
  // Truncates an existing file.
  virtual Status NewWritableFile(const std::string& path,
                                 std::unique_ptr<WritableFile>* result) = 0;
  // Opens for append, creating if needed.
  virtual Status NewAppendableFile(const std::string& path,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& path) = 0;
  virtual Status GetChildren(const std::string& dir, std::vector<std::string>* result) = 0;
  virtual Status RemoveFile(const std::string& path) = 0;
  virtual Status CreateDir(const std::string& path) = 0;
  virtual Status RemoveDir(const std::string& path) = 0;
  virtual Status GetFileSize(const std::string& path, uint64_t* size) = 0;
  virtual Status RenameFile(const std::string& from, const std::string& to) = 0;

  // Flushes the directory entry itself. A rename is not durable until the
  // directory is synced, which is what makes the CURRENT file swap safe.
  virtual Status SyncDir(const std::string& path) = 0;

  virtual Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) = 0;
};

// Forwards every call to a wrapped Env. Tests subclass this and override the
// few operations they want to interfere with.
class EnvWrapper : public Env {
 public:
  explicit EnvWrapper(Env* target) : target_(target) {}

  Env* target() const { return target_; }

  Status NewSequentialFile(const std::string& path,
                           std::unique_ptr<SequentialFile>* result) override {
    return target_->NewSequentialFile(path, result);
  }
  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* result) override {
    return target_->NewRandomAccessFile(path, result);
  }
  Status NewWritableFile(const std::string& path,
                         std::unique_ptr<WritableFile>* result) override {
    return target_->NewWritableFile(path, result);
  }
  Status NewAppendableFile(const std::string& path,
                           std::unique_ptr<WritableFile>* result) override {
    return target_->NewAppendableFile(path, result);
  }
  bool FileExists(const std::string& path) override { return target_->FileExists(path); }
  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    return target_->GetChildren(dir, result);
  }
  Status RemoveFile(const std::string& path) override { return target_->RemoveFile(path); }
  Status CreateDir(const std::string& path) override { return target_->CreateDir(path); }
  Status RemoveDir(const std::string& path) override { return target_->RemoveDir(path); }
  Status GetFileSize(const std::string& path, uint64_t* size) override {
    return target_->GetFileSize(path, size);
  }
  Status RenameFile(const std::string& from, const std::string& to) override {
    return target_->RenameFile(from, to);
  }
  Status SyncDir(const std::string& path) override { return target_->SyncDir(path); }
  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) override {
    return target_->LockFile(path, lock);
  }

 private:
  Env* target_;
};

// Convenience helpers used by recovery and the manifest.
Status WriteStringToFile(Env* env, std::string_view data, const std::string& path, bool sync);
Status ReadFileToString(Env* env, const std::string& path, std::string* data);

}  // namespace lsmkv
