#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "db/dbformat.h"
#include "harness.h"
#include "table/block.h"
#include "table/format.h"
#include "table/iterators.h"
#include "table/sstable.h"
#include "table/table_cache.h"
#include "testutil.h"
#include "util/crc32c.h"

namespace lsmkv {
namespace {

std::string Ikey(std::string_view user_key, SequenceNumber seq) {
  std::string out;
  AppendInternalKey(&out, {user_key, seq, ValueType::kValue});
  return out;
}

// Builds a table in memory and returns its bytes.
std::string BuildTable(const Options& options, const std::vector<std::pair<std::string, std::string>>& entries,
                       uint64_t* file_size) {
  std::string contents;
  test::StringSink sink(&contents);
  TableBuilder builder(options, &sink);
  for (const auto& [k, v] : entries) builder.Add(k, v);
  const Status s = builder.Finish();
  if (!s.ok()) return "";
  *file_size = builder.FileSize();
  return contents;
}

Options SmallBlocks() {
  Options options;
  options.block_size = 256;  // force many blocks out of a small table
  options.block_restart_interval = 4;
  return options;
}

}  // namespace

TEST(Block, RoundTripsEntries) {
  for (int restart_interval : {1, 4, 16}) {
    BlockBuilder builder(restart_interval);
    std::map<std::string, std::string> model;
    for (int i = 0; i < 200; ++i) {
      // Shared prefixes are what the prefix compression is for.
      const std::string key = "prefix/key" + std::string(3 - std::to_string(i % 1000).size(), '0') +
                              std::to_string(i);
      const std::string value = "value-" + std::to_string(i);
      model[key] = value;
    }
    for (const auto& [k, v] : model) builder.Add(k, v);

    std::unique_ptr<Block> block;
    CHECK_OK(Block::Create(std::string(builder.Finish()), CompareBytewise, &block));

    auto iter = block->NewIterator();
    auto expected = model.begin();
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      CHECK(expected != model.end());
      CHECK_EQ(iter->key(), std::string_view(expected->first));
      CHECK_EQ(iter->value(), std::string_view(expected->second));
      ++expected;
    }
    CHECK(expected == model.end());
    CHECK_OK(iter->status());
  }
}

TEST(Block, SeekLandsOnFirstKeyAtOrAfterTarget) {
  BlockBuilder builder(4);
  std::vector<std::string> keys;
  for (int i = 0; i < 100; ++i) keys.push_back("k" + std::to_string(i * 2));  // even numbers only
  std::sort(keys.begin(), keys.end());
  for (const auto& k : keys) builder.Add(k, "v");

  std::unique_ptr<Block> block;
  CHECK_OK(Block::Create(std::string(builder.Finish()), CompareBytewise, &block));
  auto iter = block->NewIterator();

  for (const auto& target : keys) {
    iter->Seek(target);
    CHECK(iter->Valid());
    CHECK_EQ(iter->key(), std::string_view(target));
  }
  // A target between entries lands on the next one.
  iter->Seek("k0");
  CHECK(iter->Valid());
  CHECK_EQ(iter->key(), std::string_view("k0"));
  // Past the last key the iterator goes invalid rather than wrapping.
  iter->Seek("zzz");
  CHECK(!iter->Valid());
  // Before the first key it lands on the first.
  iter->Seek("");
  CHECK(iter->Valid());
  CHECK_EQ(iter->key(), std::string_view(keys.front()));
}

TEST(Block, ReverseIterationMatchesForward) {
  BlockBuilder builder(3);
  std::vector<std::string> keys;
  for (int i = 0; i < 57; ++i) keys.push_back("key" + std::string(2 - std::to_string(i).size(), '0') + std::to_string(i));
  for (const auto& k : keys) builder.Add(k, "v" + k);

  std::unique_ptr<Block> block;
  CHECK_OK(Block::Create(std::string(builder.Finish()), CompareBytewise, &block));
  auto iter = block->NewIterator();

  std::vector<std::string> backwards;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) backwards.emplace_back(iter->key());
  CHECK_EQ(backwards.size(), keys.size());
  for (size_t i = 0; i < keys.size(); ++i) CHECK_EQ(backwards[i], keys[keys.size() - 1 - i]);

  // Seek then step backwards: the entry before the target.
  iter->Seek(keys[30]);
  iter->Prev();
  CHECK(iter->Valid());
  CHECK_EQ(iter->key(), std::string_view(keys[29]));
}

TEST(Block, EmptyBlockIteratesToNothing) {
  BlockBuilder builder(4);
  std::unique_ptr<Block> block;
  CHECK_OK(Block::Create(std::string(builder.Finish()), CompareBytewise, &block));
  auto iter = block->NewIterator();
  iter->SeekToFirst();
  CHECK(!iter->Valid());
  iter->SeekToLast();
  CHECK(!iter->Valid());
  iter->Seek("anything");
  CHECK(!iter->Valid());
  CHECK_OK(iter->status());
}

// A damaged restart count must be rejected at Create rather than sending an
// iterator off the end of the buffer.
TEST(Block, RejectsImpossibleRestartCount) {
  BlockBuilder builder(4);
  builder.Add("a", "1");
  builder.Add("b", "2");
  std::string contents(builder.Finish());

  std::string damaged = contents;
  const uint32_t huge = 0xffffff;
  std::memcpy(damaged.data() + damaged.size() - 4, &huge, sizeof(huge));
  std::unique_ptr<Block> block;
  CHECK(!Block::Create(std::move(damaged), CompareBytewise, &block).ok());

  CHECK(!Block::Create("ab", CompareBytewise, &block).ok());  // shorter than a trailer
}

TEST(Format, FooterRoundTrips) {
  BlockHandle meta, index;
  meta.set_offset(1234);
  meta.set_size(56);
  index.set_offset(9876543210ull);
  index.set_size(4096);

  Footer footer;
  footer.set_metaindex_handle(meta);
  footer.set_index_handle(index);
  std::string encoded;
  footer.EncodeTo(&encoded);
  CHECK_EQ(encoded.size(), Footer::kEncodedLength);

  Footer decoded;
  CHECK_OK(decoded.DecodeFrom(encoded));
  CHECK_EQ(decoded.metaindex_handle().offset(), 1234u);
  CHECK_EQ(decoded.metaindex_handle().size(), 56u);
  CHECK_EQ(decoded.index_handle().offset(), 9876543210ull);
  CHECK_EQ(decoded.index_handle().size(), 4096u);

  // A file that does not end with the magic number is not one of ours.
  std::string bad = encoded;
  bad[bad.size() - 1] = static_cast<char>(bad[bad.size() - 1] ^ 0xff);
  Footer rejected;
  CHECK(rejected.DecodeFrom(bad).IsCorruption());
  CHECK(rejected.DecodeFrom("short").IsCorruption());
}

TEST(Table, RoundTripsAndFindsEveryKey) {
  const Options options = SmallBlocks();
  std::vector<std::pair<std::string, std::string>> entries;
  std::map<std::string, std::string> model;
  for (int i = 0; i < 1000; ++i) {
    const std::string user_key = test::NumericKey(static_cast<uint64_t>(i));
    const std::string value = "value-" + std::to_string(i) + std::string(i % 40, 'x');
    entries.emplace_back(Ikey(user_key, static_cast<SequenceNumber>(i + 1)), value);
    model[user_key] = value;
  }

  uint64_t file_size = 0;
  const std::string contents = BuildTable(options, entries, &file_size);
  CHECK(file_size > 0);

  test::StringRandomSource file(contents);
  BlockCache cache(1 << 20);
  std::unique_ptr<Table> table;
  CHECK_OK(Table::Open(options, &file, file_size, 1, &cache, &table));

  // Every key is found by a point lookup.
  for (const auto& [user_key, value] : model) {
    std::string found;
    bool hit = false;
    const LookupKey lookup(user_key, kMaxSequenceNumber);
    CHECK_OK(table->InternalGet(ReadOptions(), lookup.internal_key(),
                                [&](std::string_view k, std::string_view v) {
                                  ParsedInternalKey parsed;
                                  if (ParseInternalKey(k, &parsed) && parsed.user_key == user_key) {
                                    found.assign(v);
                                    hit = true;
                                  }
                                }));
    CHECK(hit);
    CHECK_EQ(found, value);
  }

  // Iteration returns them in order.
  auto iter = table->NewIterator(ReadOptions());
  auto expected = model.begin();
  int seen = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    CHECK(expected != model.end());
    CHECK_EQ(ExtractUserKey(iter->key()), std::string_view(expected->first));
    CHECK_EQ(iter->value(), std::string_view(expected->second));
    ++expected;
    ++seen;
  }
  CHECK_OK(iter->status());
  CHECK_EQ(seen, 1000);

  std::printf("       (%d entries in %llu bytes, %zu cached blocks)\n", seen,
              static_cast<unsigned long long>(file_size), cache.charge() > 0 ? size_t{1} : size_t{0});
}

TEST(Table, SeekAndReverseIteration) {
  const Options options = SmallBlocks();
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 500; ++i) {
    entries.emplace_back(Ikey(test::NumericKey(static_cast<uint64_t>(i * 2)),
                              static_cast<SequenceNumber>(i + 1)),
                         "v" + std::to_string(i));
  }

  uint64_t file_size = 0;
  const std::string contents = BuildTable(options, entries, &file_size);
  test::StringRandomSource file(contents);
  BlockCache cache(1 << 20);
  std::unique_ptr<Table> table;
  CHECK_OK(Table::Open(options, &file, file_size, 1, &cache, &table));

  auto iter = table->NewIterator(ReadOptions());

  // Seeking to a key that is not present lands on the next one, across a
  // block boundary as often as not.
  for (int i = 0; i < 500; ++i) {
    const std::string target = Ikey(test::NumericKey(static_cast<uint64_t>(i * 2 + 1)), kMaxSequenceNumber);
    iter->Seek(target);
    if (i == 499) {
      CHECK(!iter->Valid());
    } else {
      CHECK(iter->Valid());
      // The expected key is held in a named local: a string_view over the
      // temporary would dangle before the comparison runs.
      const std::string expected = test::NumericKey(static_cast<uint64_t>((i + 1) * 2));
      CHECK_EQ(ExtractUserKey(iter->key()), std::string_view(expected));
    }
  }

  int backwards = 0;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) ++backwards;
  CHECK_EQ(backwards, 500);
  CHECK_OK(iter->status());
}

// The filter is the reason a miss is cheap. Without it every miss costs a
// block read, so this checks it is consulted and that it never hides a key
// that is present.
TEST(Table, BloomFilterSkipsMissesButNeverHits) {
  Options options = SmallBlocks();
  options.bloom_bits_per_key = 10;

  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 2000; ++i) {
    entries.emplace_back(Ikey(test::NumericKey(static_cast<uint64_t>(i)), static_cast<SequenceNumber>(i + 1)), "v");
  }
  uint64_t file_size = 0;
  const std::string contents = BuildTable(options, entries, &file_size);

  test::StringRandomSource file(contents);
  BlockCache cache(1 << 20);
  std::unique_ptr<Table> table;
  CHECK_OK(Table::Open(options, &file, file_size, 1, &cache, &table));

  // Present keys must never be skipped by the filter.
  for (int i = 0; i < 2000; ++i) {
    bool hit = false;
    const LookupKey lookup(test::NumericKey(static_cast<uint64_t>(i)), kMaxSequenceNumber);
    CHECK_OK(table->InternalGet(ReadOptions(), lookup.internal_key(),
                                [&](std::string_view, std::string_view) { hit = true; }));
    CHECK(hit);
  }
  CHECK_EQ(table->filter_skips(), 0u);

  // Absent keys mostly are.
  for (int i = 0; i < 2000; ++i) {
    const LookupKey lookup(test::NumericKey(static_cast<uint64_t>(5'000'000 + i)), kMaxSequenceNumber);
    CHECK_OK(table->InternalGet(ReadOptions(), lookup.internal_key(),
                                [](std::string_view, std::string_view) {}));
  }
  const uint64_t skipped = table->filter_skips();
  std::printf("       (%llu of 2000 misses answered by the filter alone)\n",
              static_cast<unsigned long long>(skipped));
  CHECK(skipped > 1900);  // a 10-bit filter should miss well under 5%
}

TEST(Table, WorksWithoutAFilter) {
  Options options = SmallBlocks();
  options.bloom_bits_per_key = 0;  // filters disabled

  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 300; ++i) {
    entries.emplace_back(Ikey(test::NumericKey(static_cast<uint64_t>(i)), static_cast<SequenceNumber>(i + 1)), "v");
  }
  uint64_t file_size = 0;
  const std::string contents = BuildTable(options, entries, &file_size);

  test::StringRandomSource file(contents);
  std::unique_ptr<Table> table;
  CHECK_OK(Table::Open(options, &file, file_size, 1, nullptr, &table));

  int found = 0;
  for (int i = 0; i < 300; ++i) {
    const LookupKey lookup(test::NumericKey(static_cast<uint64_t>(i)), kMaxSequenceNumber);
    CHECK_OK(table->InternalGet(ReadOptions(), lookup.internal_key(),
                                [&](std::string_view, std::string_view) { ++found; }));
  }
  CHECK_EQ(found, 300);
  CHECK_EQ(table->filter_skips(), 0u);
}

// Bit rot inside a data block must surface as an error, not as a wrong answer.
TEST(Table, ChecksumCatchesCorruptedBlock) {
  const Options options = SmallBlocks();
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 200; ++i) {
    entries.emplace_back(Ikey(test::NumericKey(static_cast<uint64_t>(i)), static_cast<SequenceNumber>(i + 1)),
                         "value" + std::to_string(i));
  }
  uint64_t file_size = 0;
  std::string contents = BuildTable(options, entries, &file_size);

  // Flip a bit early in the file, which lands in the first data block.
  contents[40] = static_cast<char>(contents[40] ^ 0x20);

  test::StringRandomSource file(contents);
  BlockCache cache(1 << 20);
  std::unique_ptr<Table> table;
  CHECK_OK(Table::Open(options, &file, file_size, 1, &cache, &table));

  ReadOptions read_options;
  read_options.verify_checksums = true;
  auto iter = table->NewIterator(read_options);
  bool saw_error = false;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
  }
  if (!iter->status().ok()) saw_error = true;
  CHECK(saw_error);
  CHECK(iter->status().IsCorruption());
}

TEST(Table, HandlesEmptyTableAndSingleEntry) {
  const Options options = SmallBlocks();

  uint64_t empty_size = 0;
  const std::string empty = BuildTable(options, {}, &empty_size);
  test::StringRandomSource empty_file(empty);
  std::unique_ptr<Table> empty_table;
  CHECK_OK(Table::Open(options, &empty_file, empty_size, 1, nullptr, &empty_table));
  auto empty_iter = empty_table->NewIterator(ReadOptions());
  empty_iter->SeekToFirst();
  CHECK(!empty_iter->Valid());
  CHECK_OK(empty_iter->status());

  uint64_t one_size = 0;
  const std::string one = BuildTable(options, {{Ikey("only", 1), "value"}}, &one_size);
  test::StringRandomSource one_file(one);
  std::unique_ptr<Table> one_table;
  CHECK_OK(Table::Open(options, &one_file, one_size, 2, nullptr, &one_table));
  auto one_iter = one_table->NewIterator(ReadOptions());
  one_iter->SeekToFirst();
  CHECK(one_iter->Valid());
  CHECK_EQ(one_iter->value(), std::string_view("value"));
  one_iter->Next();
  CHECK(!one_iter->Valid());
}

TEST(Table, LargeValuesSpanBlocks) {
  const Options options = SmallBlocks();  // 256-byte blocks
  std::vector<std::pair<std::string, std::string>> entries;
  for (int i = 0; i < 20; ++i) {
    // Each value is far larger than a block, so each lands in a block of
    // its own and the index has to cope with wildly uneven blocks.
    entries.emplace_back(Ikey(test::NumericKey(static_cast<uint64_t>(i)), static_cast<SequenceNumber>(i + 1)),
                         std::string(5000, static_cast<char>('a' + i)));
  }
  uint64_t file_size = 0;
  const std::string contents = BuildTable(options, entries, &file_size);

  test::StringRandomSource file(contents);
  BlockCache cache(1 << 20);
  std::unique_ptr<Table> table;
  CHECK_OK(Table::Open(options, &file, file_size, 1, &cache, &table));

  auto iter = table->NewIterator(ReadOptions());
  int seen = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    CHECK_EQ(iter->value().size(), 5000u);
    const char first = iter->value()[0];
    CHECK_EQ(first, static_cast<char>('a' + seen));
    ++seen;
  }
  CHECK_EQ(seen, 20);
  CHECK_OK(iter->status());
}

TEST(BlockCache, EvictsLeastRecentlyUsedAndReportsHits) {
  BlockCache cache(4096);

  auto make_block = [](size_t payload) {
    BlockBuilder builder(4);
    builder.Add("k", std::string(payload, 'v'));
    std::unique_ptr<Block> block;
    Block::Create(std::string(builder.Finish()), CompareBytewise, &block);
    return std::shared_ptr<Block>(std::move(block));
  };

  for (uint64_t i = 0; i < 20; ++i) cache.Insert(1, i, make_block(500));
  CHECK(cache.charge() <= 4096);

  // The most recent insert is still there; the first is long gone.
  CHECK(cache.Lookup(1, 19) != nullptr);
  CHECK(cache.Lookup(1, 0) == nullptr);
  CHECK(cache.hits() >= 1);
  CHECK(cache.misses() >= 1);

  // A block handed out before eviction stays alive for its holder.
  auto held = cache.Lookup(1, 19);
  for (uint64_t i = 100; i < 120; ++i) cache.Insert(1, i, make_block(500));
  CHECK(cache.Lookup(1, 19) == nullptr);  // evicted from the cache
  CHECK(held != nullptr);                 // but still usable here
  CHECK(held->size() > 0);

  // Dropping a table drops its blocks.
  cache.Insert(2, 1, make_block(100));
  CHECK(cache.Lookup(2, 1) != nullptr);
  cache.EraseTable(2);
  CHECK(cache.Lookup(2, 1) == nullptr);
}

TEST(MergingIterator, MergesInOrderAndPrefersEarlierChildren) {
  // Three sorted streams; the first holds the newest version of each key.
  auto make = [](const std::vector<std::pair<std::string, std::string>>& entries) {
    BlockBuilder builder(2);
    for (const auto& [k, v] : entries) builder.Add(k, v);
    auto block = std::make_shared<std::unique_ptr<Block>>();
    std::unique_ptr<Block> b;
    Block::Create(std::string(builder.Finish()), CompareBytewise, &b);
    // The iterator must outlive the block, so hold it alongside.
    struct Holder final : Iterator {
      std::unique_ptr<Block> block;
      std::unique_ptr<Iterator> iter;
      bool Valid() const override { return iter->Valid(); }
      void SeekToFirst() override { iter->SeekToFirst(); }
      void SeekToLast() override { iter->SeekToLast(); }
      void Seek(std::string_view t) override { iter->Seek(t); }
      void Next() override { iter->Next(); }
      void Prev() override { iter->Prev(); }
      std::string_view key() const override { return iter->key(); }
      std::string_view value() const override { return iter->value(); }
      Status status() const override { return iter->status(); }
    };
    auto holder = std::make_unique<Holder>();
    holder->iter = b->NewIterator();
    holder->block = std::move(b);
    return std::unique_ptr<Iterator>(std::move(holder));
  };

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(make({{"a", "new"}, {"c", "new"}}));
  children.push_back(make({{"a", "old"}, {"b", "only"}}));
  children.push_back(make({{"b", "older"}, {"d", "only"}}));

  auto merged = NewMergingIterator(std::move(children), CompareBytewise);

  std::vector<std::pair<std::string, std::string>> seen;
  for (merged->SeekToFirst(); merged->Valid(); merged->Next()) {
    seen.emplace_back(merged->key(), merged->value());
  }
  // Every version appears, newest first within a key.
  const std::vector<std::pair<std::string, std::string>> expected = {
      {"a", "new"}, {"a", "old"}, {"b", "only"}, {"b", "older"}, {"c", "new"}, {"d", "only"}};
  CHECK_EQ(seen.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK_EQ(seen[i].first, expected[i].first);
    CHECK_EQ(seen[i].second, expected[i].second);
  }

  // Reverse iteration gives the same sequence backwards.
  std::vector<std::pair<std::string, std::string>> backwards;
  for (merged->SeekToLast(); merged->Valid(); merged->Prev()) {
    backwards.emplace_back(merged->key(), merged->value());
  }
  CHECK_EQ(backwards.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    CHECK_EQ(backwards[i].first, expected[expected.size() - 1 - i].first);
    CHECK_EQ(backwards[i].second, expected[expected.size() - 1 - i].second);
  }

  // Changing direction mid-scan has to reposition every other child.
  merged->SeekToFirst();
  merged->Next();
  merged->Next();
  CHECK_EQ(merged->key(), std::string_view("b"));
  merged->Prev();
  CHECK_EQ(merged->key(), std::string_view("a"));
  CHECK_EQ(merged->value(), std::string_view("old"));
  merged->Next();
  CHECK_EQ(merged->key(), std::string_view("b"));
  CHECK_EQ(merged->value(), std::string_view("only"));
}

}  // namespace lsmkv
