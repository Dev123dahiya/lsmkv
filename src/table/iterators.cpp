#include "table/iterators.h"

#include <cassert>
#include <utility>

namespace lsmkv {
namespace {

class EmptyIterator final : public Iterator {
 public:
  explicit EmptyIterator(Status s) : status_(std::move(s)) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(std::string_view) override {}
  void Next() override { assert(false); }
  void Prev() override { assert(false); }
  std::string_view key() const override {
    assert(false);
    return {};
  }
  std::string_view value() const override {
    assert(false);
    return {};
  }
  Status status() const override { return status_; }

 private:
  Status status_;
};

class TwoLevelIterator final : public Iterator {
 public:
  TwoLevelIterator(std::unique_ptr<Iterator> index,
                   std::function<std::unique_ptr<Iterator>(std::string_view)> open)
      : index_iter_(std::move(index)), open_(std::move(open)) {}

  bool Valid() const override { return data_iter_ != nullptr && data_iter_->Valid(); }

  std::string_view key() const override { return data_iter_->key(); }
  std::string_view value() const override { return data_iter_->value(); }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToFirst();
    SkipEmptyDataBlocksForward();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->SeekToLast();
    SkipEmptyDataBlocksBackward();
  }

  void Seek(std::string_view target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_ != nullptr) data_iter_->Seek(target);
    SkipEmptyDataBlocksForward();
  }

  void Next() override {
    assert(Valid());
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  void Prev() override {
    assert(Valid());
    data_iter_->Prev();
    SkipEmptyDataBlocksBackward();
  }

  Status status() const override {
    if (!index_iter_->status().ok()) return index_iter_->status();
    if (data_iter_ != nullptr && !data_iter_->status().ok()) return data_iter_->status();
    return status_;
  }

 private:
  void SaveError(const Status& s) {
    if (status_.ok() && !s.ok()) status_ = s;
  }

  // An index entry can name a block with no entries in range, so moving on
  // may have to cross several blocks before landing on one.
  void SkipEmptyDataBlocksForward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (data_iter_ != nullptr) SaveError(data_iter_->status());
      if (!index_iter_->Valid()) {
        data_iter_.reset();
        return;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_ != nullptr) data_iter_->SeekToFirst();
    }
  }

  void SkipEmptyDataBlocksBackward() {
    while (data_iter_ == nullptr || !data_iter_->Valid()) {
      if (data_iter_ != nullptr) SaveError(data_iter_->status());
      if (!index_iter_->Valid()) {
        data_iter_.reset();
        return;
      }
      index_iter_->Prev();
      InitDataBlock();
      if (data_iter_ != nullptr) data_iter_->SeekToLast();
    }
  }

  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      data_iter_.reset();
      return;
    }
    const std::string_view handle = index_iter_->value();
    if (data_iter_ != nullptr && handle == current_handle_) return;  // already open
    current_handle_.assign(handle);
    data_iter_ = open_(current_handle_);
    if (data_iter_ != nullptr) SaveError(data_iter_->status());
  }

  std::unique_ptr<Iterator> index_iter_;
  std::function<std::unique_ptr<Iterator>(std::string_view)> open_;
  std::unique_ptr<Iterator> data_iter_;
  std::string current_handle_;
  Status status_;
};

// Merges children with a linear scan for the smallest key. The child count
// is the number of memtables plus level-0 files plus one per deeper level —
// small enough that a heap would cost more in bookkeeping than it saves.
class MergingIterator final : public Iterator {
 public:
  MergingIterator(std::vector<std::unique_ptr<Iterator>> children, BlockKeyComparator cmp)
      : children_(std::move(children)), compare_(cmp) {}

  bool Valid() const override { return current_ != nullptr; }

  std::string_view key() const override {
    assert(Valid());
    return current_->key();
  }
  std::string_view value() const override {
    assert(Valid());
    return current_->value();
  }

  Status status() const override {
    for (const auto& child : children_) {
      if (!child->status().ok()) return child->status();
    }
    return Status::OK();
  }

  void SeekToFirst() override {
    for (auto& child : children_) child->SeekToFirst();
    FindSmallest();
    direction_ = Direction::kForward;
  }

  void SeekToLast() override {
    for (auto& child : children_) child->SeekToLast();
    FindLargest();
    direction_ = Direction::kReverse;
  }

  void Seek(std::string_view target) override {
    for (auto& child : children_) child->Seek(target);
    FindSmallest();
    direction_ = Direction::kForward;
  }

  void Next() override {
    assert(Valid());
    // Changing direction: every other child is positioned at or before the
    // current key and has to be moved past it first.
    if (direction_ != Direction::kForward) {
      const std::string saved_key(current_->key());
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->Seek(saved_key);
        if (child->Valid() && compare_(saved_key, child->key()) == 0) child->Next();
      }
      direction_ = Direction::kForward;
    }
    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    assert(Valid());
    if (direction_ != Direction::kReverse) {
      const std::string saved_key(current_->key());
      for (auto& child : children_) {
        if (child.get() == current_) continue;
        child->Seek(saved_key);
        if (child->Valid()) {
          child->Prev();  // step back to the last key before the target
        } else {
          child->SeekToLast();
        }
      }
      direction_ = Direction::kReverse;
    }
    current_->Prev();
    FindLargest();
  }

 private:
  enum class Direction { kForward, kReverse };

  void FindSmallest() {
    Iterator* smallest = nullptr;
    // Scanning in order means an earlier child wins ties, which is how
    // newer versions of a key take precedence.
    for (auto& child : children_) {
      if (!child->Valid()) continue;
      if (smallest == nullptr || compare_(child->key(), smallest->key()) < 0) {
        smallest = child.get();
      }
    }
    current_ = smallest;
  }

  void FindLargest() {
    Iterator* largest = nullptr;
    // Reverse order so that on a tie the earliest child still wins.
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
      if (!(*it)->Valid()) continue;
      if (largest == nullptr || compare_((*it)->key(), largest->key()) > 0) largest = it->get();
    }
    current_ = largest;
  }

  std::vector<std::unique_ptr<Iterator>> children_;
  const BlockKeyComparator compare_;
  Iterator* current_ = nullptr;
  Direction direction_ = Direction::kForward;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() {
  return std::make_unique<EmptyIterator>(Status::OK());
}

std::unique_ptr<Iterator> NewErrorIterator(Status status) {
  return std::make_unique<EmptyIterator>(std::move(status));
}

std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index,
    std::function<std::unique_ptr<Iterator>(std::string_view)> open) {
  return std::make_unique<TwoLevelIterator>(std::move(index), std::move(open));
}

std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children,
                                             BlockKeyComparator cmp) {
  if (children.empty()) return NewEmptyIterator();
  if (children.size() == 1) return std::move(children[0]);
  return std::make_unique<MergingIterator>(std::move(children), cmp);
}

}  // namespace lsmkv
