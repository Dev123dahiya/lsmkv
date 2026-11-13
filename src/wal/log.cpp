#include "wal/log.h"

#include <cstring>

#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv::log {

Status Writer::AddRecord(std::string_view data) {
  const char* ptr = data.data();
  size_t left = data.size();

  // A zero-length record is legal and must still produce one physical
  // record, so the loop always runs at least once.
  Status s;
  bool begin = true;
  do {
    const size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      if (leftover > 0) {
        // Not enough room for a header: pad the block out with zeroes, which
        // the reader recognises as a trailer.
        static constexpr char kPad[kHeaderSize] = {};
        LSMKV_TRY(dest_->Append(std::string_view(kPad, leftover)));
      }
      block_offset_ = 0;
    }

    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length = left < avail ? left : avail;
    const bool end = (left == fragment_length);

    RecordType type;
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType type, const char* ptr, size_t length) {
  char header[kHeaderSize];
  header[4] = static_cast<char>(length & 0xff);
  header[5] = static_cast<char>(length >> 8);
  header[6] = static_cast<char>(type);

  // The checksum covers the type byte as well as the payload, so a flipped
  // type is caught.
  uint32_t crc = crc32c::Extend(0, &header[6], 1);
  crc = crc32c::Extend(crc, ptr, length);
  const uint32_t masked = crc32c::Mask(crc);
  std::memcpy(header, &masked, sizeof(masked));

  LSMKV_TRY(dest_->Append(std::string_view(header, kHeaderSize)));
  LSMKV_TRY(dest_->Append(std::string_view(ptr, length)));
  LSMKV_TRY(dest_->Flush());
  block_offset_ += kHeaderSize + length;
  return Status::OK();
}

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum, uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(std::make_unique<char[]>(kBlockSize)),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

bool Reader::SkipToInitialBlock() {
  const size_t offset_in_block = static_cast<size_t>(initial_offset_ % kBlockSize);
  uint64_t block_start_location = initial_offset_ - offset_in_block;

  // Landing in a block trailer means the record of interest starts in the
  // next block.
  if (offset_in_block > kBlockSize - kHeaderSize) block_start_location += kBlockSize;

  end_of_buffer_offset_ = block_start_location;

  if (block_start_location > 0) {
    const Status skip_status = file_->Skip(block_start_location);
    if (!skip_status.ok()) {
      ReportDrop(block_start_location, skip_status);
      return false;
    }
  }
  return true;
}

bool Reader::ReadRecord(std::string_view* record, std::string* scratch) {
  if (last_record_offset_ < initial_offset_ && !SkipToInitialBlock()) return false;

  scratch->clear();
  *record = {};
  bool in_fragmented_record = false;
  uint64_t prospective_record_offset = 0;

  std::string_view fragment;
  while (true) {
    const unsigned record_type = ReadPhysicalRecord(&fragment);

    // Offset of the fragment just read.
    const uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

    if (resyncing_) {
      // Started mid-record: skip fragments until a record actually begins.
      if (record_type == kMiddleType) continue;
      if (record_type == kLastType) {
        resyncing_ = false;
        continue;
      }
      resyncing_ = false;
    }

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          // A first fragment with no last fragment: the earlier write was
          // torn. Report it unless the partial record is empty.
          if (!scratch->empty()) ReportCorruption(scratch->size(), "partial record without end");
        }
        prospective_record_offset = physical_record_offset;
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;

      case kFirstType:
        if (in_fragmented_record) {
          if (!scratch->empty()) ReportCorruption(scratch->size(), "partial record without end");
        }
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "missing start of fragmented record");
        } else {
          scratch->append(fragment);
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "missing start of fragmented record");
        } else {
          scratch->append(fragment);
          *record = *scratch;
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;

      case kEof:
        if (in_fragmented_record) {
          // A record cut short by a crash. The data was never acknowledged
          // to a caller, so dropping it is correct, not a corruption.
          scratch->clear();
        }
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default: {
        const size_t dropped = fragment.size() + (in_fragmented_record ? scratch->size() : 0);
        ReportCorruption(dropped, "unknown record type");
        in_fragmented_record = false;
        scratch->clear();
        break;
      }
    }
  }
}

unsigned Reader::ReadPhysicalRecord(std::string_view* result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        buffer_ = {};
        std::string_view read_result;
        const Status status = file_->Read(kBlockSize, &read_result, backing_store_.get());
        buffer_ = read_result;
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) {
          buffer_ = {};
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        }
        if (buffer_.size() < kBlockSize) eof_ = true;
        continue;
      }
      // A trailer shorter than a header at end of file is the normal zero
      // padding, not corruption.
      buffer_ = {};
      return kEof;
    }

    const char* header = buffer_.data();
    const auto a = static_cast<uint32_t>(static_cast<uint8_t>(header[4]));
    const auto b = static_cast<uint32_t>(static_cast<uint8_t>(header[5]));
    const auto type = static_cast<uint8_t>(header[6]);
    const uint32_t length = a | (b << 8);

    if (kHeaderSize + length > buffer_.size()) {
      const size_t drop_size = buffer_.size();
      buffer_ = {};
      if (!eof_) {
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }
      // A header whose payload never arrived: the process died mid-write.
      return kEof;
    }

    if (type == kZeroType && length == 0) {
      // Zero padding inside a block. Skip the rest of the block.
      buffer_ = {};
      return kBadRecord;
    }

    if (checksum_) {
      const uint32_t expected = crc32c::Unmask(DecodeFixed32(header));
      uint32_t actual = crc32c::Extend(0, header + 6, 1);
      actual = crc32c::Extend(actual, header + kHeaderSize, length);
      if (actual != expected) {
        // Drop the rest of the block: trusting the length after a failed
        // checksum could walk off into arbitrary bytes.
        const size_t drop_size = buffer_.size();
        buffer_ = {};
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    buffer_.remove_prefix(kHeaderSize + length);

    // A record entirely before the requested start offset is skipped.
    if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length < initial_offset_) {
      *result = {};
      return kBadRecord;
    }

    *result = std::string_view(header + kHeaderSize, length);
    return type;
  }
}

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != nullptr && end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}

}  // namespace lsmkv::log
