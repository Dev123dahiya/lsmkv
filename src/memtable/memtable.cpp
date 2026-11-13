#include "memtable/memtable.h"

#include <cassert>
#include <cstring>

#include "util/coding.h"

namespace lsmkv {
namespace {

// Reads the length-prefixed internal key an entry starts with. The pointer
// form of the varint decoder is used so nothing past the entry is touched,
// which matters because an entry can sit at the very end of an arena block.
std::string_view DecodeEntryKey(const char* entry) {
  uint32_t len = 0;
  const char* p = GetVarint32Ptr(entry, &len);
  return {p, len};
}

std::string_view DecodeEntryValue(const char* entry) {
  const std::string_view key = DecodeEntryKey(entry);
  uint32_t len = 0;
  const char* p = GetVarint32Ptr(key.data() + key.size(), &len);
  return {p, len};
}

}  // namespace

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  return CompareInternalKey(DecodeEntryKey(a), DecodeEntryKey(b));
}

MemTable::MemTable() : table_(KeyComparator(), &arena_) {}

void MemTable::Add(SequenceNumber seq, ValueType type, std::string_view key,
                   std::string_view value) {
  const size_t key_size = key.size();
  const size_t val_size = value.size();
  const size_t internal_key_size = key_size + 8;
  const size_t encoded_len = static_cast<size_t>(VarintLength(internal_key_size)) +
                             internal_key_size +
                             static_cast<size_t>(VarintLength(val_size)) + val_size;

  char* buf = arena_.Allocate(encoded_len);
  char* p = buf;

  // Inline varint writes so the whole entry lands in one arena allocation.
  auto put_varint32 = [](char* dst, uint32_t v) {
    while (v >= 128) {
      *dst++ = static_cast<char>(static_cast<uint8_t>(v) | 0x80);
      v >>= 7;
    }
    *dst++ = static_cast<char>(v);
    return dst;
  };

  p = put_varint32(p, static_cast<uint32_t>(internal_key_size));
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  const uint64_t tag = PackTag(seq, type);
  std::memcpy(p, &tag, sizeof(tag));
  p += sizeof(tag);
  p = put_varint32(p, static_cast<uint32_t>(val_size));
  if (val_size > 0) std::memcpy(p, value.data(), val_size);
  p += val_size;

  assert(static_cast<size_t>(p - buf) == encoded_len);
  (void)p;

  table_.Insert(buf);
}

bool MemTable::Get(const LookupKey& key, std::string* value, Status* s) const {
  const std::string_view memkey = key.memtable_key();
  Table::Iterator iter(&table_);
  iter.Seek(memkey.data());
  if (!iter.Valid()) return false;

  // The seek lands on the first entry >= (user_key, max tag), which is the
  // newest visible version of the key if there is one.
  const std::string_view found = DecodeEntryKey(iter.key());
  if (ExtractUserKey(found) != key.user_key()) return false;

  ParsedInternalKey parsed;
  if (!ParseInternalKey(found, &parsed)) {
    *s = Status::Corruption("malformed key in memtable");
    return true;
  }
  switch (parsed.type) {
    case ValueType::kValue: {
      const std::string_view v = DecodeEntryValue(iter.key());
      value->assign(v.data(), v.size());
      *s = Status::OK();
      return true;
    }
    case ValueType::kDeletion:
      // A tombstone is an answer: stop here rather than consulting older
      // tables that may still hold the overwritten value.
      *s = Status::NotFound("");
      return true;
  }
  *s = Status::Corruption("unknown value type in memtable");
  return true;
}

namespace {

class MemTableIterator final : public Iterator {
 public:
  explicit MemTableIterator(const SkipList<const char*, MemTable::KeyComparator>* table)
      : iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }
  void SeekToFirst() override { iter_.SeekToFirst(); }
  void SeekToLast() override { iter_.SeekToLast(); }
  void Next() override { iter_.Next(); }
  void Prev() override { iter_.Prev(); }

  void Seek(std::string_view target) override {
    // The skip list compares length-prefixed entries, so the bare internal
    // key has to be wrapped before seeking.
    tmp_.clear();
    PutVarint32(&tmp_, static_cast<uint32_t>(target.size()));
    tmp_.append(target);
    iter_.Seek(tmp_.data());
  }

  std::string_view key() const override { return DecodeEntryKey(iter_.key()); }
  std::string_view value() const override { return DecodeEntryValue(iter_.key()); }
  Status status() const override { return Status::OK(); }

 private:
  SkipList<const char*, MemTable::KeyComparator>::Iterator iter_;
  std::string tmp_;
};

}  // namespace

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace lsmkv
