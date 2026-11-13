// Access to a WriteBatch's encoding, used by the log and the memtable. Kept
// out of the public header so the layout stays an implementation detail.
#pragma once

#include <cstring>
#include <string_view>

#include "db/dbformat.h"
#include "lsmkv/write_batch.h"

namespace lsmkv {

class MemTable;

// fixed64 sequence | fixed32 count
constexpr size_t kWriteBatchHeaderSize = 12;

class WriteBatchInternal {
 public:
  // The sequence number of the batch's first record; later records in the
  // same batch take consecutive numbers.
  static SequenceNumber Sequence(const WriteBatch* b);
  static void SetSequence(WriteBatch* b, SequenceNumber seq);

  static uint32_t Count(const WriteBatch* b) { return b->Count(); }
  static void SetCount(WriteBatch* b, uint32_t count);

  static std::string_view Contents(const WriteBatch* b);
  static void SetContents(WriteBatch* b, std::string_view contents);

  static size_t ByteSize(const WriteBatch* b) { return b->ApproximateSize(); }

  static Status InsertInto(const WriteBatch* batch, MemTable* table);
};

}  // namespace lsmkv
