// The iterator interface shared by memtables, tables, the merging iterator
// and the database itself.
//
// An iterator is positioned at an entry or is invalid. key() and value() may
// only be called while Valid(). The returned views point into memory owned by
// the iterator and are invalidated by any move, so a caller that needs to
// keep them must copy.
#pragma once

#include <string_view>

#include "lsmkv/status.h"

namespace lsmkv {

class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  virtual bool Valid() const = 0;

  virtual void SeekToFirst() = 0;
  virtual void SeekToLast() = 0;

  // Positions at the first entry with key >= target, or becomes invalid.
  virtual void Seek(std::string_view target) = 0;

  virtual void Next() = 0;
  virtual void Prev() = 0;

  virtual std::string_view key() const = 0;
  virtual std::string_view value() const = 0;

  // An iterator that hits a read or checksum error becomes invalid and
  // reports it here, so a loop over Valid() never silently stops short
  // without the caller being able to find out why.
  virtual Status status() const = 0;
};

}  // namespace lsmkv
