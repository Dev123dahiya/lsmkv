// Status: a small, allocation-free-on-success error type.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace lsmkv {

enum class Code : uint8_t {
  kOk = 0,
  kNotFound = 1,
  kCorruption = 2,
  kIOError = 3,
  kInvalidArgument = 4,
  kNotSupported = 5,
};

// A Status is either OK (no allocation, no message) or an error carrying a
// code and a heap-allocated message. Copying an error Status copies the
// message; moving one is cheap. Success is the common case, so it costs a
// single null pointer.
class Status {
 public:
  Status() noexcept = default;

  Status(const Status& other) : state_(other.state_ ? std::make_unique<State>(*other.state_) : nullptr) {}
  Status& operator=(const Status& other) {
    if (this != &other) {
      state_ = other.state_ ? std::make_unique<State>(*other.state_) : nullptr;
    }
    return *this;
  }
  Status(Status&&) noexcept = default;
  Status& operator=(Status&&) noexcept = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg = "") { return Status(Code::kNotFound, msg); }
  static Status Corruption(std::string_view msg) { return Status(Code::kCorruption, msg); }
  static Status IOError(std::string_view msg) { return Status(Code::kIOError, msg); }
  static Status InvalidArgument(std::string_view msg) { return Status(Code::kInvalidArgument, msg); }
  static Status NotSupported(std::string_view msg) { return Status(Code::kNotSupported, msg); }

  bool ok() const noexcept { return state_ == nullptr; }
  bool IsNotFound() const noexcept { return code() == Code::kNotFound; }
  bool IsCorruption() const noexcept { return code() == Code::kCorruption; }
  bool IsIOError() const noexcept { return code() == Code::kIOError; }

  Code code() const noexcept { return state_ ? state_->code : Code::kOk; }
  const std::string& message() const noexcept {
    static const std::string kEmpty;
    return state_ ? state_->message : kEmpty;
  }

  std::string ToString() const {
    if (ok()) return "OK";
    std::string out;
    switch (state_->code) {
      case Code::kNotFound: out = "NotFound"; break;
      case Code::kCorruption: out = "Corruption"; break;
      case Code::kIOError: out = "IOError"; break;
      case Code::kInvalidArgument: out = "InvalidArgument"; break;
      case Code::kNotSupported: out = "NotSupported"; break;
      case Code::kOk: out = "OK"; break;
    }
    if (!state_->message.empty()) {
      out += ": ";
      out += state_->message;
    }
    return out;
  }

 private:
  struct State {
    Code code;
    std::string message;
  };

  Status(Code code, std::string_view msg)
      : state_(std::make_unique<State>(State{code, std::string(msg)})) {}

  std::unique_ptr<State> state_;
};

// Returns from the enclosing function if `expr` yields a non-OK Status.
#define LSMKV_TRY(expr)                        \
  do {                                         \
    ::lsmkv::Status _lsmkv_s = (expr);         \
    if (!_lsmkv_s.ok()) return _lsmkv_s;       \
  } while (0)

}  // namespace lsmkv
