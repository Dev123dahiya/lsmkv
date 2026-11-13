#include "util/env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace lsmkv {
namespace {

Status PosixError(const std::string& context, int err) {
  const std::string msg = context + ": " + std::strerror(err);
  // ENOENT is how callers detect a missing file, so it keeps its own code.
  if (err == ENOENT) return Status::NotFound(msg);
  return Status::IOError(msg);
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    while (true) {
      const ssize_t r = ::read(fd_, scratch, n);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(path_, errno);
      }
      *result = std::string_view(scratch, static_cast<size_t>(r));
      return Status::OK();
    }
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) == static_cast<off_t>(-1)) {
      return PosixError(path_, errno);
    }
    return Status::OK();
  }

 private:
  const std::string path_;
  const int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string_view* result,
              char* scratch) const override {
    // pread does not move the file offset, so concurrent reads are safe
    // without a lock.
    size_t done = 0;
    while (done < n) {
      const ssize_t r =
          ::pread(fd_, scratch + done, n - done, static_cast<off_t>(offset + done));
      if (r < 0) {
        if (errno == EINTR) continue;
        *result = std::string_view(scratch, done);
        return PosixError(path_, errno);
      }
      if (r == 0) break;  // end of file
      done += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, done);
    return Status::OK();
  }

 private:
  const std::string path_;
  const int fd_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) {
      // Best effort: a file dropped without Close has already lost its
      // error reporting, so there is nothing useful to return here.
      FlushBuffer();
      ::close(fd_);
    }
  }

  Status Append(std::string_view data) override {
    size_t copy = std::min(data.size(), kBufferSize - pos_);
    std::memcpy(buf_ + pos_, data.data(), copy);
    pos_ += copy;
    data.remove_prefix(copy);
    if (data.empty()) return Status::OK();

    LSMKV_TRY(FlushBuffer());

    // Large writes bypass the buffer entirely.
    if (data.size() >= kBufferSize) return WriteAll(data.data(), data.size());

    std::memcpy(buf_, data.data(), data.size());
    pos_ = data.size();
    return Status::OK();
  }

  Status Flush() override { return FlushBuffer(); }

  Status Sync() override {
    LSMKV_TRY(FlushBuffer());
    if (::fdatasync(fd_) != 0) return PosixError(path_, errno);
    return Status::OK();
  }

  Status Close() override {
    Status s = FlushBuffer();
    const int fd = fd_;
    fd_ = -1;
    if (::close(fd) != 0 && s.ok()) s = PosixError(path_, errno);
    return s;
  }

 private:
  static constexpr size_t kBufferSize = 65536;

  Status FlushBuffer() {
    if (pos_ == 0) return Status::OK();
    const Status s = WriteAll(buf_, pos_);
    pos_ = 0;
    return s;
  }

  Status WriteAll(const char* data, size_t n) {
    while (n > 0) {
      const ssize_t r = ::write(fd_, data, n);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(path_, errno);
      }
      data += r;
      n -= static_cast<size_t>(r);
    }
    return Status::OK();
  }

  const std::string path_;
  int fd_;
  char buf_[kBufferSize];
  size_t pos_ = 0;
};

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixFileLock() override {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }

 private:
  const std::string path_;
  const int fd_;
};

class PosixEnv final : public Env {
 public:
  Status NewSequentialFile(const std::string& path,
                           std::unique_ptr<SequentialFile>* result) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(path, errno);
    *result = std::make_unique<PosixSequentialFile>(path, fd);
    return Status::OK();
  }

  Status NewRandomAccessFile(const std::string& path,
                             std::unique_ptr<RandomAccessFile>* result) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(path, errno);
    *result = std::make_unique<PosixRandomAccessFile>(path, fd);
    return Status::OK();
  }

  Status NewWritableFile(const std::string& path,
                         std::unique_ptr<WritableFile>* result) override {
    const int fd = ::open(path.c_str(), O_TRUNC | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(path, errno);
    *result = std::make_unique<PosixWritableFile>(path, fd);
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& path,
                           std::unique_ptr<WritableFile>* result) override {
    const int fd = ::open(path.c_str(), O_APPEND | O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(path, errno);
    *result = std::make_unique<PosixWritableFile>(path, fd);
    return Status::OK();
  }

  bool FileExists(const std::string& path) override {
    return ::access(path.c_str(), F_OK) == 0;
  }

  Status GetChildren(const std::string& dir, std::vector<std::string>* result) override {
    result->clear();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return PosixError(dir, errno);
    while (struct dirent* entry = ::readdir(d)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") result->push_back(name);
    }
    ::closedir(d);
    return Status::OK();
  }

  Status RemoveFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) return PosixError(path, errno);
    return Status::OK();
  }

  Status CreateDir(const std::string& path) override {
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return PosixError(path, errno);
    return Status::OK();
  }

  Status RemoveDir(const std::string& path) override {
    if (::rmdir(path.c_str()) != 0) return PosixError(path, errno);
    return Status::OK();
  }

  Status GetFileSize(const std::string& path, uint64_t* size) override {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
      *size = 0;
      return PosixError(path, errno);
    }
    *size = static_cast<uint64_t>(st.st_size);
    return Status::OK();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) return PosixError(from, errno);
    return Status::OK();
  }

  Status SyncDir(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(path, errno);
    Status s;
    if (::fsync(fd) != 0) s = PosixError(path, errno);
    ::close(fd);
    return s;
  }

  Status LockFile(const std::string& path, std::unique_ptr<FileLock>* lock) override {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(path, errno);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      const int err = errno;
      ::close(fd);
      return Status::IOError("database is locked by another process: " + path + ": " +
                             std::strerror(err));
    }
    *lock = std::make_unique<PosixFileLock>(path, fd);
    return Status::OK();
  }
};

}  // namespace

Env* Env::Default() {
  // Leaked on purpose: files opened through it may outlive static
  // destruction order.
  static Env* env = new PosixEnv();
  return env;
}

Status WriteStringToFile(Env* env, std::string_view data, const std::string& path, bool sync) {
  std::unique_ptr<WritableFile> file;
  LSMKV_TRY(env->NewWritableFile(path, &file));
  Status s = file->Append(data);
  if (s.ok() && sync) s = file->Sync();
  if (s.ok()) {
    s = file->Close();
  } else {
    file->Close();  // already failing; keep the first error
  }
  if (!s.ok()) env->RemoveFile(path);
  return s;
}

Status ReadFileToString(Env* env, const std::string& path, std::string* data) {
  data->clear();
  std::unique_ptr<SequentialFile> file;
  LSMKV_TRY(env->NewSequentialFile(path, &file));

  constexpr size_t kChunk = 8192;
  std::string scratch(kChunk, '\0');
  while (true) {
    std::string_view fragment;
    LSMKV_TRY(file->Read(kChunk, &fragment, scratch.data()));
    if (fragment.empty()) break;
    data->append(fragment);
  }
  return Status::OK();
}

}  // namespace lsmkv
