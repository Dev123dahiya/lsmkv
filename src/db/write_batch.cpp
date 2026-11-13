#include "lsmkv/write_batch.h"

#include <cassert>

#include "db/dbformat.h"
#include "db/write_batch_internal.h"
#include "memtable/memtable.h"
#include "util/coding.h"

namespace lsmkv {

WriteBatch::WriteBatch() { rep_.resize(kWriteBatchHeaderSize, '\0'); }

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kWriteBatchHeaderSize, '\0');
}

uint32_t WriteBatch::Count() const { return DecodeFixed32(rep_.data() + 8); }

void WriteBatch::Put(std::string_view key, std::string_view value) {
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kValue));
  PutLengthPrefixed(&rep_, key);
  PutLengthPrefixed(&rep_, value);
}

void WriteBatch::Delete(std::string_view key) {
  WriteBatchInternal::SetCount(this, Count() + 1);
  rep_.push_back(static_cast<char>(ValueType::kDeletion));
  PutLengthPrefixed(&rep_, key);
}

void WriteBatch::Append(const WriteBatch& source) {
  WriteBatchInternal::SetCount(this, Count() + source.Count());
  rep_.append(source.rep_, kWriteBatchHeaderSize, std::string::npos);
}

Status WriteBatch::Iterate(Handler* handler) const {
  if (rep_.size() < kWriteBatchHeaderSize) {
    return Status::Corruption("write batch is shorter than its header");
  }

  std::string_view input(rep_);
  input.remove_prefix(kWriteBatchHeaderSize);

  uint32_t found = 0;
  while (!input.empty()) {
    const auto tag = static_cast<uint8_t>(input[0]);
    input.remove_prefix(1);
    ++found;

    std::string_view key, value;
    switch (static_cast<ValueType>(tag)) {
      case ValueType::kValue:
        if (!GetLengthPrefixed(&input, &key) || !GetLengthPrefixed(&input, &value)) {
          return Status::Corruption("write batch: truncated put");
        }
        handler->Put(key, value);
        break;
      case ValueType::kDeletion:
        if (!GetLengthPrefixed(&input, &key)) {
          return Status::Corruption("write batch: truncated delete");
        }
        handler->Delete(key);
        break;
      default:
        return Status::Corruption("write batch: unknown record type");
    }
  }
  // A count that disagrees with the records means the batch was damaged in
  // a way the per-record decoding happened not to notice.
  if (found != Count()) return Status::Corruption("write batch count does not match its records");
  return Status::OK();
}

SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return DecodeFixed64(b->rep_.data());
}

void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  std::memcpy(b->rep_.data(), &seq, sizeof(seq));
}

void WriteBatchInternal::SetCount(WriteBatch* b, uint32_t count) {
  std::memcpy(b->rep_.data() + 8, &count, sizeof(count));
}

std::string_view WriteBatchInternal::Contents(const WriteBatch* b) { return b->rep_; }

void WriteBatchInternal::SetContents(WriteBatch* b, std::string_view contents) {
  assert(contents.size() >= kWriteBatchHeaderSize);
  b->rep_.assign(contents);
}

namespace {

// Applies a batch to a memtable, giving each record its own sequence number
// so that two writes of the same key inside one batch keep their order.
class MemTableInserter final : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber sequence, MemTable* table)
      : sequence_(sequence), table_(table) {}

  void Put(std::string_view key, std::string_view value) override {
    table_->Add(sequence_++, ValueType::kValue, key, value);
  }
  void Delete(std::string_view key) override {
    table_->Add(sequence_++, ValueType::kDeletion, key, "");
  }

 private:
  SequenceNumber sequence_;
  MemTable* table_;
};

}  // namespace

Status WriteBatchInternal::InsertInto(const WriteBatch* batch, MemTable* table) {
  MemTableInserter inserter(Sequence(batch), table);
  return batch->Iterate(&inserter);
}

}  // namespace lsmkv
