#include "table/table_cache.h"

#include <cstdio>

#include "table/iterators.h"

namespace lsmkv {
namespace {

std::string MakeFileName(const std::string& dbname, uint64_t number, const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/%06llu.%s", static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

}  // namespace

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "tbl");
}
std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "log");
}
std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu", static_cast<unsigned long long>(number));
  return dbname + buf;
}
std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }
std::string LockFileName(const std::string& dbname) { return dbname + "/LOCK"; }
std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "tmp");
}

std::shared_ptr<Block> BlockCache::Lookup(uint64_t cache_id, uint64_t offset) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = index_.find(Key{cache_id, offset});
  if (it == index_.end()) {
    ++misses_;
    return nullptr;
  }
  ++hits_;
  lru_.splice(lru_.begin(), lru_, it->second);  // move to front
  return it->second->block;
}

void BlockCache::Insert(uint64_t cache_id, uint64_t offset, std::shared_ptr<Block> block) {
  if (capacity_ == 0 || block == nullptr) return;
  // The map entry and list node cost something too; counting only the block
  // would let a cache of tiny blocks use far more memory than its capacity.
  const size_t charge = block->size() + sizeof(Entry) + sizeof(Key) + 48;

  std::lock_guard<std::mutex> lock(mu_);
  const Key key{cache_id, offset};
  if (const auto it = index_.find(key); it != index_.end()) {
    charge_ -= it->second->charge;
    lru_.erase(it->second);
    index_.erase(it);
  }
  lru_.push_front(Entry{key, std::move(block), charge});
  index_[key] = lru_.begin();
  charge_ += charge;
  EvictLocked();
}

void BlockCache::EvictLocked() {
  while (charge_ > capacity_ && !lru_.empty()) {
    const auto& victim = lru_.back();
    charge_ -= victim.charge;
    index_.erase(victim.key);
    // The shared_ptr may still be held by an iterator; the block lives until
    // that iterator is destroyed.
    lru_.pop_back();
  }
}

void BlockCache::EraseTable(uint64_t cache_id) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto it = lru_.begin(); it != lru_.end();) {
    if (it->key.cache_id == cache_id) {
      charge_ -= it->charge;
      index_.erase(it->key);
      it = lru_.erase(it);
    } else {
      ++it;
    }
  }
}

uint64_t BlockCache::hits() const {
  std::lock_guard<std::mutex> lock(mu_);
  return hits_;
}
uint64_t BlockCache::misses() const {
  std::lock_guard<std::mutex> lock(mu_);
  return misses_;
}
size_t BlockCache::charge() const {
  std::lock_guard<std::mutex> lock(mu_);
  return charge_;
}

TableCache::TableCache(std::string dbname, const Options& options, Env* env, size_t capacity)
    : dbname_(std::move(dbname)),
      options_(options),
      env_(env),
      capacity_(capacity == 0 ? 1 : capacity),
      block_cache_(options.block_cache_bytes) {}

TableCache::~TableCache() = default;

Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             std::shared_ptr<TableEntry>* entry) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (const auto it = index_.find(file_number); it != index_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second);
      *entry = *it->second;
      return Status::OK();
    }
  }

  // Opening is done outside the lock: it touches the filesystem, and two
  // threads opening different tables should not serialise.
  const std::string path = TableFileName(dbname_, file_number);
  auto fresh = std::make_shared<TableEntry>();
  fresh->file_number = file_number;
  LSMKV_TRY(env_->NewRandomAccessFile(path, &fresh->file));

  uint64_t cache_id;
  {
    std::lock_guard<std::mutex> lock(mu_);
    cache_id = next_cache_id_++;
  }
  fresh->cache_id = cache_id;
  LSMKV_TRY(Table::Open(options_, fresh->file.get(), file_size, cache_id, &block_cache_,
                        &fresh->table));

  std::lock_guard<std::mutex> lock(mu_);
  ++opens_;
  // Another thread may have opened the same table meanwhile; keep theirs so
  // there is only ever one entry per file.
  if (const auto it = index_.find(file_number); it != index_.end()) {
    lru_.splice(lru_.begin(), lru_, it->second);
    *entry = *it->second;
    return Status::OK();
  }
  lru_.push_front(std::move(fresh));
  index_[file_number] = lru_.begin();
  *entry = lru_.front();

  while (index_.size() > capacity_ && lru_.size() > 1) {
    const auto& victim = lru_.back();
    block_cache_.EraseTable(victim->cache_id);
    index_.erase(victim->file_number);
    lru_.pop_back();  // the shared_ptr keeps it alive for any live iterator
  }
  return Status::OK();
}

std::unique_ptr<Iterator> TableCache::NewIterator(const ReadOptions& options,
                                                  uint64_t file_number, uint64_t file_size) {
  std::shared_ptr<TableEntry> entry;
  const Status s = FindTable(file_number, file_size, &entry);
  if (!s.ok()) return NewErrorIterator(s);

  // Holds the table open for as long as the iterator lives, even if the
  // cache evicts it.
  class TableHoldingIterator final : public Iterator {
   public:
    TableHoldingIterator(std::shared_ptr<TableEntry> entry, std::unique_ptr<Iterator> iter)
        : entry_(std::move(entry)), iter_(std::move(iter)) {}

    bool Valid() const override { return iter_->Valid(); }
    void SeekToFirst() override { iter_->SeekToFirst(); }
    void SeekToLast() override { iter_->SeekToLast(); }
    void Seek(std::string_view target) override { iter_->Seek(target); }
    void Next() override { iter_->Next(); }
    void Prev() override { iter_->Prev(); }
    std::string_view key() const override { return iter_->key(); }
    std::string_view value() const override { return iter_->value(); }
    Status status() const override { return iter_->status(); }

   private:
    std::shared_ptr<TableEntry> entry_;
    std::unique_ptr<Iterator> iter_;
  };

  auto inner = entry->table->NewIterator(options);
  return std::make_unique<TableHoldingIterator>(std::move(entry), std::move(inner));
}

Status TableCache::Get(
    const ReadOptions& options, uint64_t file_number, uint64_t file_size, std::string_view key,
    const std::function<void(std::string_view, std::string_view)>& handle_result) {
  std::shared_ptr<TableEntry> entry;
  LSMKV_TRY(FindTable(file_number, file_size, &entry));
  return entry->table->InternalGet(options, key, handle_result);
}

void TableCache::Evict(uint64_t file_number) {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = index_.find(file_number);
  if (it == index_.end()) return;
  block_cache_.EraseTable((*it->second)->cache_id);
  lru_.erase(it->second);
  index_.erase(it);
}

uint64_t TableCache::opens() const {
  std::lock_guard<std::mutex> lock(mu_);
  return opens_;
}

}  // namespace lsmkv
