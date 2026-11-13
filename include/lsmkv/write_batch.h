// A batch of updates applied atomically: either every update in the batch is
// visible after a crash, or none is. The batch is also the unit written to
// the log, so grouping updates cuts the number of log records and syncs.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

class WriteBatch {
 public:
  WriteBatch();

  void Put(std::string_view key, std::string_view value);
  void Delete(std::string_view key);

  // Drops every update, keeping the allocated buffer for reuse.
  void Clear();

  // Number of updates in the batch.
  uint32_t Count() const;

  // Bytes the batch will add to the log.
  size_t ApproximateSize() const { return rep_.size(); }

  // Appends every update of `source` to this batch.
  void Append(const WriteBatch& source);

  // Receives the updates in a batch in order.
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual void Put(std::string_view key, std::string_view value) = 0;
    virtual void Delete(std::string_view key) = 0;
  };

  // Replays the batch into `handler`. Returns Corruption if the encoding is
  // malformed, which is how a truncated log record is caught.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  // Layout: fixed64(sequence) | fixed32(count) | records
  // record := kTypeValue   varstring(key) varstring(value)
  //         | kTypeDeletion varstring(key)
  std::string rep_;
};

}  // namespace lsmkv
