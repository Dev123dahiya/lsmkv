// A bump allocator for memtable nodes. Allocations are never freed
// individually; the whole arena is released when the memtable is dropped.
// This is what lets skip-list nodes be handed to readers without reference
// counting: nothing moves and nothing is freed while the memtable is alive.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lsmkv {

class Arena {
 public:
  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  char* Allocate(size_t bytes) {
    // A zero-byte request still returns a distinct, usable pointer.
    if (bytes == 0) bytes = 1;
    if (bytes <= remaining_) {
      char* result = next_;
      next_ += bytes;
      remaining_ -= bytes;
      return result;
    }
    return AllocateFallback(bytes);
  }

  // Allocates with alignment suitable for any scalar up to a pointer.
  char* AllocateAligned(size_t bytes) {
    constexpr size_t kAlign = alignof(std::max_align_t) > 8 ? 8 : alignof(std::max_align_t);
    static_assert((kAlign & (kAlign - 1)) == 0, "alignment must be a power of two");
    const size_t slop = reinterpret_cast<uintptr_t>(next_) & (kAlign - 1);
    const size_t padding = slop == 0 ? 0 : kAlign - slop;
    if (bytes + padding <= remaining_) {
      char* result = next_ + padding;
      next_ += bytes + padding;
      remaining_ -= bytes + padding;
      return result;
    }
    // A fresh block from the allocator is already suitably aligned.
    return AllocateFallback(bytes);
  }

  // Total heap held by the arena, including the unused tail of the current
  // block. Readable from other threads, which is why it is atomic.
  size_t MemoryUsage() const noexcept { return usage_.load(std::memory_order_relaxed); }

 private:
  static constexpr size_t kBlockSize = 4096;

  char* AllocateFallback(size_t bytes) {
    if (bytes > kBlockSize / 4) {
      // Give an oversized request its own block rather than wasting the tail
      // of the current one.
      return AllocateNewBlock(bytes);
    }
    next_ = AllocateNewBlock(kBlockSize);
    remaining_ = kBlockSize - bytes;
    char* result = next_;
    next_ += bytes;
    return result;
  }

  char* AllocateNewBlock(size_t size) {
    blocks_.push_back(std::make_unique<char[]>(size));
    usage_.fetch_add(size + sizeof(std::unique_ptr<char[]>), std::memory_order_relaxed);
    return blocks_.back().get();
  }

  char* next_ = nullptr;
  size_t remaining_ = 0;
  std::vector<std::unique_ptr<char[]>> blocks_;
  std::atomic<size_t> usage_{0};
};

}  // namespace lsmkv
