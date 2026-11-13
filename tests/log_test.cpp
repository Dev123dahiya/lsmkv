#include <random>
#include <string>
#include <vector>

#include "harness.h"
#include "testutil.h"
#include "util/crc32c.h"
#include "wal/log.h"

namespace lsmkv {
namespace {

class CountingReporter final : public log::Reader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    ++count_;
    dropped_ += bytes;
    last_ = status;
  }
  int count() const { return count_; }
  size_t dropped() const { return dropped_; }
  const Status& last() const { return last_; }

 private:
  int count_ = 0;
  size_t dropped_ = 0;
  Status last_;
};

// Writes `records` to a log and returns the file bytes.
std::string WriteLog(const std::vector<std::string>& records) {
  std::string contents;
  test::StringSink sink(&contents);
  log::Writer writer(&sink);
  for (const auto& r : records) {
    const Status s = writer.AddRecord(r);
    if (!s.ok()) return "";
  }
  return contents;
}

// Reads every record from `contents`, appending corruption reports to
// `reporter`.
std::vector<std::string> ReadLog(std::string_view contents, CountingReporter* reporter,
                                 bool checksum = true) {
  test::StringSource source(contents);
  log::Reader reader(&source, reporter, checksum, 0);
  std::vector<std::string> out;
  std::string scratch;
  std::string_view record;
  while (reader.ReadRecord(&record, &scratch)) out.emplace_back(record);
  return out;
}

}  // namespace

TEST(Log, EmptyFileYieldsNoRecords) {
  CountingReporter reporter;
  const auto records = ReadLog("", &reporter);
  CHECK_EQ(records.size(), 0u);
  CHECK_EQ(reporter.count(), 0);
}

TEST(Log, RoundTripsSmallRecords) {
  const std::vector<std::string> written = {"", "a", "bar", std::string(100, 'x'), "", "foo"};
  CountingReporter reporter;
  const auto read = ReadLog(WriteLog(written), &reporter);
  CHECK_EQ(reporter.count(), 0);
  CHECK_EQ(read.size(), written.size());
  for (size_t i = 0; i < written.size(); ++i) CHECK_EQ(read[i], written[i]);
}

// Records at and around the block boundary are where the fragmenting logic
// lives, so each of those sizes gets its own case.
TEST(Log, RoundTripsRecordsAroundBlockBoundaries) {
  std::mt19937 rng(99);
  std::vector<std::string> written;
  for (size_t len : {log::kBlockSize - log::kHeaderSize - 1, log::kBlockSize - log::kHeaderSize,
                     log::kBlockSize - log::kHeaderSize + 1, log::kBlockSize - 1, log::kBlockSize,
                     log::kBlockSize + 1, 2 * log::kBlockSize, 3 * log::kBlockSize + 17}) {
    written.push_back(test::RandomString(rng, len));
    written.push_back("short");  // force a new record right after
  }
  CountingReporter reporter;
  const auto read = ReadLog(WriteLog(written), &reporter);
  CHECK_EQ(reporter.count(), 0);
  CHECK_EQ(read.size(), written.size());
  for (size_t i = 0; i < written.size(); ++i) {
    CHECK_EQ(read[i].size(), written[i].size());
    CHECK(read[i] == written[i]);
  }
}

TEST(Log, RoundTripsManyRandomRecords) {
  std::mt19937 rng(2024);
  std::vector<std::string> written;
  for (int i = 0; i < 500; ++i) {
    const size_t len = (i % 50 == 0) ? rng() % 70000 : rng() % 200;
    written.push_back(test::RandomString(rng, len));
  }
  CountingReporter reporter;
  const auto read = ReadLog(WriteLog(written), &reporter);
  CHECK_EQ(reporter.count(), 0);
  CHECK_EQ(read.size(), written.size());
  for (size_t i = 0; i < written.size(); ++i) CHECK(read[i] == written[i]);
}

// The point of the format: a file cut at any byte must still yield every
// record that was completely written, with no corruption reported for the
// torn tail.
TEST(Log, TruncationAtAnyByteKeepsCompleteRecords) {
  std::mt19937 rng(7);
  std::vector<std::string> written;
  for (int i = 0; i < 40; ++i) written.push_back(test::RandomString(rng, rng() % 3000));
  const std::string full = WriteLog(written);

  // Where each record ends in the file, so a truncation length can be mapped
  // to the number of records that must survive.
  std::vector<size_t> complete_after;
  {
    std::string partial_contents;
    test::StringSink sink(&partial_contents);
    log::Writer writer(&sink);
    for (const auto& r : written) {
      CHECK_OK(writer.AddRecord(r));
      complete_after.push_back(partial_contents.size());
    }
  }

  int checked = 0;
  for (size_t cut = 0; cut <= full.size(); cut += 97) {
    CountingReporter reporter;
    const auto read = ReadLog(std::string_view(full.data(), cut), &reporter);

    size_t expected = 0;
    while (expected < complete_after.size() && complete_after[expected] <= cut) ++expected;

    CHECK_EQ(read.size(), expected);
    for (size_t i = 0; i < expected; ++i) CHECK(read[i] == written[i]);
    // A torn tail is a crash, not corruption. Nothing should be reported.
    CHECK_EQ(reporter.count(), 0);
    ++checked;
  }
  std::printf("       (%d truncation points, file %zu bytes)\n", checked, full.size());
}

// A flipped bit inside a payload must be caught by the checksum rather than
// handed back to the caller.
TEST(Log, ChecksumCatchesCorruptedPayload) {
  const std::vector<std::string> written = {"first record", "second record", "third record"};
  std::string contents = WriteLog(written);

  // Byte 7 is the first payload byte of the first record.
  contents[log::kHeaderSize] = static_cast<char>(contents[log::kHeaderSize] ^ 0x40);

  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  CHECK_EQ(reporter.count(), 1);
  CHECK(reporter.last().IsCorruption());
  // The damaged record is dropped. The rest of the block is dropped with it,
  // because a length read after a failed checksum cannot be trusted.
  CHECK_EQ(read.size(), 0u);
}

// The checksum covers the type byte and the length, not just the payload, so
// a header damaged in place is caught rather than acted on.
TEST(Log, ChecksumCatchesCorruptedHeader) {
  for (size_t byte : {size_t{4}, size_t{6}}) {
    std::string contents = WriteLog({"a record long enough to notice", "another"});
    contents[byte] = static_cast<char>(contents[byte] ^ 0x01);  // stays within the file
    CountingReporter reporter;
    const auto read = ReadLog(contents, &reporter);
    CHECK(reporter.count() >= 1);
    CHECK(reporter.last().IsCorruption());
    CHECK_EQ(read.size(), 0u);
  }
}

// A length that points past the end of the file is exactly what a crash
// mid-write leaves behind, so the reader stops there without reporting
// corruption. Damage to that field is indistinguishable from a torn write:
// the reader cannot tell the two apart, and the records after it are lost
// either way. Recovery relies on the rest of the file being rewritten, not on
// detecting this case.
TEST(Log, LengthPastEndOfFileIsTreatedAsATornWrite) {
  std::string contents = WriteLog({"a record long enough to notice", "another"});
  contents[4] = static_cast<char>(contents[4] ^ 0x80);  // length jumps past the file

  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  CHECK_EQ(read.size(), 0u);
  CHECK_EQ(reporter.count(), 0);  // silent, by design

  // Mid-file the same damage is caught, because the length no longer runs
  // past what has been read.
  std::string padded = contents;
  padded.resize(log::kBlockSize * 2, '\0');
  CountingReporter padded_reporter;
  ReadLog(padded, &padded_reporter);
  CHECK(padded_reporter.count() >= 1);
}

// With checksums off the reader must not crash on damaged input, even though
// it can no longer detect the damage.
TEST(Log, SurvivesCorruptionWithChecksumsDisabled) {
  std::string contents = WriteLog({"hello", "world"});
  contents[log::kHeaderSize] = 'H';
  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter, /*checksum=*/false);
  CHECK_EQ(read.size(), 2u);
  CHECK_EQ(read[0], std::string("Hello"));
}

// A writer that opens an existing log must line its fragments up with the
// block structure already on disk.
TEST(Log, ResumesWritingIntoAPartiallyFilledBlock) {
  std::mt19937 rng(5);
  const std::vector<std::string> first_batch = {test::RandomString(rng, 5000),
                                                test::RandomString(rng, 300)};
  std::string contents = WriteLog(first_batch);

  const std::vector<std::string> second_batch = {test::RandomString(rng, 40000), "tail"};
  {
    test::StringSink sink(&contents);
    log::Writer writer(&sink, contents.size());
    for (const auto& r : second_batch) CHECK_OK(writer.AddRecord(r));
  }

  CountingReporter reporter;
  const auto read = ReadLog(contents, &reporter);
  CHECK_EQ(reporter.count(), 0);
  CHECK_EQ(read.size(), first_batch.size() + second_batch.size());
  CHECK(read[0] == first_batch[0]);
  CHECK(read[2] == second_batch[0]);
  CHECK(read[3] == second_batch[1]);
}

TEST(Log, ReportsLastRecordOffset) {
  const std::vector<std::string> written = {"one", "two", std::string(40000, 'z'), "four"};
  const std::string contents = WriteLog(written);

  test::StringSource source(contents);
  CountingReporter reporter;
  log::Reader reader(&source, &reporter, true, 0);

  std::string scratch;
  std::string_view record;
  uint64_t previous = 0;
  int count = 0;
  while (reader.ReadRecord(&record, &scratch)) {
    if (count > 0) CHECK(reader.LastRecordOffset() > previous);
    previous = reader.LastRecordOffset();
    ++count;
  }
  CHECK_EQ(count, 4);
  CHECK(previous < contents.size());
}

}  // namespace lsmkv
