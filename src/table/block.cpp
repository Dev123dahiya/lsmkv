#include "table/block.h"

#include <algorithm>
#include <cassert>

#include "db/dbformat.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {

int CompareBytewise(std::string_view a, std::string_view b) { return a.compare(b); }
int CompareInternal(std::string_view a, std::string_view b) { return CompareInternalKey(a, b); }

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(std::string_view* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) return Status::OK();
  return Status::Corruption("bad block handle");
}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  // Pad to a fixed width so the footer can be found by seeking back from the
  // end of the file.
  dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength, '\0');
  PutFixed64(dst, kTableMagicNumber);
  assert(dst->size() == original_size + kEncodedLength);
}

Status Footer::DecodeFrom(std::string_view input) {
  if (input.size() < kEncodedLength) return Status::Corruption("file is too short to be a table");
  const char* magic_ptr = input.data() + kEncodedLength - 8;
  if (DecodeFixed64(magic_ptr) != kTableMagicNumber) {
    return Status::Corruption("not an lsmkv table file (bad magic number)");
  }
  std::string_view handles = input.substr(0, kEncodedLength - 8);
  LSMKV_TRY(metaindex_handle_.DecodeFrom(&handles));
  LSMKV_TRY(index_handle_.DecodeFrom(&handles));
  return Status::OK();
}

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
  assert(restart_interval >= 1);
  restarts_.push_back(0);  // the first entry is always a restart point
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  assert(!finished_);
  assert(counter_ <= restart_interval_);

  size_t shared = 0;
  if (counter_ < restart_interval_) {
    const size_t min_length = std::min(last_key_.size(), key.size());
    while (shared < min_length && last_key_[shared] == key[shared]) ++shared;
  } else {
    // Start a new restart point: store the key in full.
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value);

  last_key_.assign(key);
  ++counter_;
}

std::string_view BlockBuilder::Finish() {
  for (const uint32_t restart : restarts_) PutFixed32(&buffer_, restart);
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return buffer_;
}

Status Block::Create(std::string contents, BlockKeyComparator cmp,
                     std::unique_ptr<Block>* result) {
  if (contents.size() < sizeof(uint32_t)) return Status::Corruption("block is too short");

  const uint32_t num_restarts = DecodeFixed32(contents.data() + contents.size() - 4);
  // Guard before multiplying: a corrupt count must not overflow into a small
  // offset that then looks valid.
  const size_t max_restarts = (contents.size() - sizeof(uint32_t)) / sizeof(uint32_t);
  if (num_restarts > max_restarts) return Status::Corruption("block restart count is too large");

  const auto restart_offset =
      static_cast<uint32_t>(contents.size() - (1 + num_restarts) * sizeof(uint32_t));
  result->reset(new Block(std::move(contents), cmp, num_restarts, restart_offset));
  return Status::OK();
}

// Walks the entries of a block. The current key is rebuilt in `key_` as the
// iterator moves, since each entry stores only its suffix.
class Block::Iter final : public Iterator {
 public:
  Iter(BlockKeyComparator cmp, const char* data, uint32_t restart_offset, uint32_t num_restarts)
      : compare_(cmp),
        data_(data),
        restart_offset_(restart_offset),
        num_restarts_(num_restarts),
        current_(restart_offset),
        restart_index_(num_restarts) {}

  bool Valid() const override { return current_ < restart_offset_; }
  Status status() const override { return status_; }

  std::string_view key() const override {
    assert(Valid());
    return key_;
  }
  std::string_view value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    ParseNextKey();
  }

  void Prev() override {
    assert(Valid());
    // Entries carry no back pointer, so step back to the restart point
    // before the current entry and walk forward from there.
    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        current_ = restart_offset_;  // moved before the first entry
        restart_index_ = num_restarts_;
        return;
      }
      --restart_index_;
    }
    SeekToRestartPoint(restart_index_);
    while (ParseNextKey() && NextEntryOffset() < original) {
    }
  }

  void Seek(std::string_view target) override {
    // Binary search the restart points for the last one whose key is < target.
    uint32_t left = 0;
    uint32_t right = num_restarts_ == 0 ? 0 : num_restarts_ - 1;
    while (left < right) {
      const uint32_t mid = (left + right + 1) / 2;
      const uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared = 0, non_shared = 0, value_length = 0;
      const char* key_ptr = DecodeEntry(data_ + region_offset, data_ + restart_offset_, &shared,
                                        &non_shared, &value_length);
      if (key_ptr == nullptr || shared != 0) {
        CorruptionError();
        return;
      }
      if (compare_(std::string_view(key_ptr, non_shared), target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    // Then scan forward. At most restart_interval entries are examined.
    SeekToRestartPoint(left);
    while (true) {
      if (!ParseNextKey()) return;
      if (compare_(key_, target) >= 0) return;
    }
  }

  void SeekToFirst() override {
    SeekToRestartPoint(0);
    ParseNextKey();
  }

  void SeekToLast() override {
    SeekToRestartPoint(num_restarts_ == 0 ? 0 : num_restarts_ - 1);
    while (ParseNextKey() && NextEntryOffset() < restart_offset_) {
    }
  }

 private:
  // Offset of the entry after the current one.
  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  uint32_t GetRestartPoint(uint32_t index) const {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restart_offset_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    const uint32_t offset = num_restarts_ == 0 ? restart_offset_ : GetRestartPoint(index);
    // value_ is used only to compute the next entry's offset here.
    value_ = std::string_view(data_ + offset, 0);
  }

  void CorruptionError() {
    current_ = restart_offset_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = {};
  }

  bool ParseNextKey() {
    current_ = NextEntryOffset();
    const char* p = data_ + current_;
    const char* limit = data_ + restart_offset_;
    if (p >= limit) {
      current_ = restart_offset_;
      restart_index_ = num_restarts_;
      return false;
    }

    uint32_t shared = 0, non_shared = 0, value_length = 0;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    if (p == nullptr || key_.size() < shared) {
      CorruptionError();
      return false;
    }
    key_.resize(shared);
    key_.append(p, non_shared);
    value_ = std::string_view(p + non_shared, value_length);
    while (restart_index_ + 1 < num_restarts_ && GetRestartPoint(restart_index_ + 1) < current_) {
      ++restart_index_;
    }
    return true;
  }

  // Decodes the three varint lengths at `p`, returning a pointer to the key
  // suffix, or nullptr if the entry runs past `limit`.
  static const char* DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                                 uint32_t* non_shared, uint32_t* value_length) {
    if (limit - p < 3) return nullptr;
    // The common case: all three lengths fit in one byte each.
    *shared = static_cast<uint32_t>(static_cast<uint8_t>(p[0]));
    *non_shared = static_cast<uint32_t>(static_cast<uint8_t>(p[1]));
    *value_length = static_cast<uint32_t>(static_cast<uint8_t>(p[2]));
    if ((*shared | *non_shared | *value_length) < 128) {
      p += 3;
    } else {
      std::string_view in(p, static_cast<size_t>(limit - p));
      if (!GetVarint32(&in, shared)) return nullptr;
      if (!GetVarint32(&in, non_shared)) return nullptr;
      if (!GetVarint32(&in, value_length)) return nullptr;
      p = in.data();
    }
    if (static_cast<size_t>(limit - p) < static_cast<size_t>(*non_shared) + *value_length) {
      return nullptr;
    }
    return p;
  }

  const BlockKeyComparator compare_;
  const char* const data_;
  const uint32_t restart_offset_;
  const uint32_t num_restarts_;

  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  std::string_view value_;
  Status status_;
};

std::unique_ptr<Iterator> Block::NewIterator() const {
  if (num_restarts_ == 0) {
    // An empty block still needs a valid iterator; it just never becomes
    // positioned.
    return std::make_unique<Iter>(compare_, data_.data(), restart_offset_, 0);
  }
  return std::make_unique<Iter>(compare_, data_.data(), restart_offset_, num_restarts_);
}

}  // namespace lsmkv
