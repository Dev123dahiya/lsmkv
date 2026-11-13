// The record log used by both the write-ahead log and the manifest.
//
// The file is a sequence of 32 KiB blocks. A record never straddles a block
// boundary without saying so: a record too large for the space left is split
// into first/middle/last fragments, each with its own header and checksum.
// That is what makes recovery from a torn write possible — a reader can
// always resynchronise at the next block boundary instead of scanning bytes.
//
//   block  := record* trailer?
//   record := checksum (4B, masked CRC32C of type and payload)
//             length   (2B, little-endian payload size)
//             type     (1B)
//             payload  (length bytes)
//
// A block with fewer than 7 bytes left is zero-padded, and the reader skips
// such a trailer.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lsmkv/status.h"
#include "util/env.h"

namespace lsmkv::log {

enum RecordType : uint8_t {
  // A block's zero padding decodes as this, which marks the end of the
  // records in that block rather than a corrupt header.
  kZeroType = 0,
  kFullType = 1,
  // A record split across blocks.
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

constexpr int kMaxRecordType = kLastType;
constexpr size_t kBlockSize = 32768;
constexpr size_t kHeaderSize = 4 + 2 + 1;

class Writer {
 public:
  // `dest` must be positioned at the start of the file.
  explicit Writer(WritableFile* dest) : dest_(dest) {}

  // Appends a record for an existing file whose length is already known, so
  // fragments line up with the block structure already on disk.
  Writer(WritableFile* dest, uint64_t dest_length)
      : dest_(dest), block_offset_(static_cast<size_t>(dest_length % kBlockSize)) {}

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  Status AddRecord(std::string_view data);

 private:
  Status EmitPhysicalRecord(RecordType type, const char* ptr, size_t length);

  WritableFile* dest_;
  size_t block_offset_ = 0;
};

class Reader {
 public:
  // Receives each corruption the reader finds. Recovery logs them; tests
  // count them.
  class Reporter {
   public:
    virtual ~Reporter() = default;
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // `checksum` verifies every record's CRC. `initial_offset` skips to the
  // first record at or after that byte position.
  Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset);

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Reads the next record into `scratch` and points `record` at it. Returns
  // false at end of file, or when the remaining bytes are an incomplete
  // record — the signature of a crash mid-write, which is not an error.
  bool ReadRecord(std::string_view* record, std::string* scratch);

  // Byte offset just past the last record returned.
  uint64_t LastRecordOffset() const { return last_record_offset_; }

 private:
  // Values above kMaxRecordType so they cannot collide with a real type.
  enum : unsigned {
    kEof = kMaxRecordType + 1,
    kBadRecord = kMaxRecordType + 2,
  };

  bool SkipToInitialBlock();
  unsigned ReadPhysicalRecord(std::string_view* result);
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  SequentialFile* const file_;
  Reporter* const reporter_;
  const bool checksum_;
  std::unique_ptr<char[]> backing_store_;
  std::string_view buffer_;
  bool eof_ = false;

  uint64_t last_record_offset_ = 0;
  uint64_t end_of_buffer_offset_ = 0;
  const uint64_t initial_offset_;
  bool resyncing_;
};

}  // namespace lsmkv::log
