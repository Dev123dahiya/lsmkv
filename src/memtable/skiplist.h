// A skip list supporting one writer and any number of concurrent readers
// without locks.
//
// The guarantee is narrow and worth stating precisely: readers never take a
// lock and never block the writer, but only one thread may call Insert at a
// time. The database enforces that with a write mutex.
//
// Correctness rests on two rules:
//   * Nodes are arena-allocated and never freed or moved while the list is
//     alive, so a reader holding a Node* can always dereference it.
//   * A node becomes visible to readers through a release store to a
//     predecessor's link, matched by an acquire load in Next(). Everything
//     written to the node before that store is visible to a reader that
//     observes it.
#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

#include "util/arena.h"

namespace lsmkv {

// Comparator must provide `int operator()(const Key&, const Key&) const`
// returning <0, 0 or >0.
template <typename Key, class Comparator>
class SkipList {
 public:
  SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Inserts `key`, which must not already be in the list. Caller holds the
  // write lock.
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  // Estimated node count. Exact under the single-writer rule, but a reader on
  // another thread may observe a value that is already stale.
  size_t ApproximateCount() const { return count_.load(std::memory_order_relaxed); }

 private:
  struct Node;

  static constexpr int kMaxHeight = 12;
  static constexpr unsigned kBranching = 4;

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  // True when `n` sorts strictly before `key`. A null node is the end of the
  // list, which sorts after everything.
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return n != nullptr && compare_(n->key, key) < 0;
  }

  // Lowest node >= key. When `prev` is non-null it is filled with the last
  // node before `key` at each level, which Insert needs for splicing.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;
  Node* FindLessThan(const Key& key) const;
  Node* FindLast() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;

  // Written only by the inserting thread, read by iterators on other threads.
  std::atomic<int> max_height_{1};
  std::atomic<size_t> count_{0};

  // xorshift state for the level coin flips. Only the writer touches it.
  uint32_t rnd_state_ = 0x2545f491;

 public:
  // A snapshot-free cursor: it reads the live list, so entries inserted after
  // it was created may or may not be seen. The memtable only iterates lists
  // that are immutable or under the write lock, so that is not observable.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    bool Valid() const { return node_ != nullptr; }
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }
    void Prev() {
      // Walking back through links would need a doubly linked list; searching
      // costs the same O(log n) and keeps nodes smaller.
      assert(Valid());
      node_ = list_->FindLessThan(node_->key);
      if (node_ == list_->head_) node_ = nullptr;
    }
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }
    void SeekToFirst() { node_ = list_->head_->Next(0); }
    void SeekToLast() {
      node_ = list_->FindLast();
      if (node_ == list_->head_) node_ = nullptr;
    }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };
};

template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  // Acquire: pairs with the release store that published the node.
  Node* Next(int n) const {
    assert(n >= 0);
    return Link(n).load(std::memory_order_acquire);
  }
  // Release: everything written to `x` before this is visible to a reader
  // that loads this link.
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    Link(n).store(x, std::memory_order_release);
  }

  // Safe only before the node is reachable by readers, or for a link the
  // caller is about to publish with a release store.
  Node* NoBarrierNext(int n) const { return Link(n).load(std::memory_order_relaxed); }
  void NoBarrierSetNext(int n, Node* x) { Link(n).store(x, std::memory_order_relaxed); }

 private:
  // The links are allocated immediately after the node in the same arena
  // block, so one allocation covers a node of any height.
  std::atomic<Node*>& Link(int n) const {
    auto* base = reinterpret_cast<char*>(const_cast<Node*>(this)) + sizeof(Node);
    return reinterpret_cast<std::atomic<Node*>*>(base)[n];
  }
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                            int height) {
  static_assert(sizeof(Node) % alignof(std::atomic<Node*>) == 0,
                "the trailing link array must land on its natural alignment");
  char* mem = arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height));
  auto* node = new (mem) Node(key);
  auto* links = reinterpret_cast<std::atomic<Node*>*>(mem + sizeof(Node));
  for (int i = 0; i < height; ++i) new (&links[i]) std::atomic<Node*>(nullptr);
  return node;
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp), arena_(arena), head_(NewNode(Key{}, kMaxHeight)) {}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // xorshift32, then promote with probability 1/kBranching per level.
  auto next = [this] {
    rnd_state_ ^= rnd_state_ << 13;
    rnd_state_ ^= rnd_state_ >> 17;
    rnd_state_ ^= rnd_state_ << 5;
    return rnd_state_;
  };
  int height = 1;
  while (height < kMaxHeight && (next() % kBranching) == 0) ++height;
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // keep going on this level
    } else {
      if (prev != nullptr) prev[level] = x;
      if (level == 0) return next;
      --level;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLessThan(
    const Key& key) const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next != nullptr && compare_(next->key, key) < 0) {
      x = next;
    } else {
      if (level == 0) return x;
      --level;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
  Node* x = head_;
  int level = max_height_.load(std::memory_order_relaxed) - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next != nullptr) {
      x = next;
    } else {
      if (level == 0) return x;
      --level;
    }
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);
  assert(x == nullptr || !Equal(key, x->key));  // duplicates are a caller bug
  (void)x;

  const int height = RandomHeight();
  const int current = max_height_.load(std::memory_order_relaxed);
  if (height > current) {
    for (int i = current; i < height; ++i) prev[i] = head_;
    // A reader that sees the old height simply searches from a lower level
    // and still finds every node, so this store needs no extra ordering.
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* node = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // Relaxed: this node is not reachable yet, and the release store below
    // publishes these writes along with it.
    node->NoBarrierSetNext(i, prev[i]->NoBarrierNext(i));
    prev[i]->SetNext(i, node);
  }
  count_.fetch_add(1, std::memory_order_relaxed);
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && Equal(key, x->key);
}

}  // namespace lsmkv
