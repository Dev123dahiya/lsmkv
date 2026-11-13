#include <atomic>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "harness.h"
#include "memtable/memtable.h"
#include "memtable/skiplist.h"
#include "util/arena.h"

namespace lsmkv {
namespace {

struct U64Comparator {
  int operator()(const uint64_t& a, const uint64_t& b) const {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
  }
};

using TestList = SkipList<uint64_t, U64Comparator>;

std::string Ikey(std::string_view user_key, SequenceNumber seq, ValueType type) {
  std::string out;
  AppendInternalKey(&out, {user_key, seq, type});
  return out;
}

}  // namespace

TEST(SkipList, EmptyListFindsNothing) {
  Arena arena;
  TestList list(U64Comparator(), &arena);
  CHECK(!list.Contains(10));

  TestList::Iterator iter(&list);
  CHECK(!iter.Valid());
  iter.SeekToFirst();
  CHECK(!iter.Valid());
  iter.SeekToLast();
  CHECK(!iter.Valid());
  iter.Seek(100);
  CHECK(!iter.Valid());
}

// The list must behave exactly like an ordered set, including for seeks that
// land between elements and at the ends.
TEST(SkipList, MatchesOrderedSet) {
  constexpr int kInserts = 4000;
  constexpr uint64_t kRange = 2000;

  Arena arena;
  TestList list(U64Comparator(), &arena);
  std::set<uint64_t> model;
  std::mt19937_64 rng(301);

  for (int i = 0; i < kInserts; ++i) {
    const uint64_t key = rng() % kRange;
    if (model.insert(key).second) list.Insert(key);  // the list forbids duplicates
  }

  for (uint64_t k = 0; k < kRange; ++k) CHECK_EQ(list.Contains(k), model.count(k) == 1);

  // Forward iteration.
  TestList::Iterator iter(&list);
  auto model_it = model.begin();
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
    CHECK(model_it != model.end());
    CHECK_EQ(iter.key(), *model_it);
    ++model_it;
  }
  CHECK(model_it == model.end());

  // Backward iteration.
  auto model_rit = model.rbegin();
  for (iter.SeekToLast(); iter.Valid(); iter.Prev()) {
    CHECK(model_rit != model.rend());
    CHECK_EQ(iter.key(), *model_rit);
    ++model_rit;
  }
  CHECK(model_rit == model.rend());

  // Seek must land on the first key >= target for every possible target,
  // including one past the end.
  for (uint64_t target = 0; target <= kRange; ++target) {
    iter.Seek(target);
    const auto expected = model.lower_bound(target);
    if (expected == model.end()) {
      CHECK(!iter.Valid());
    } else {
      CHECK(iter.Valid());
      CHECK_EQ(iter.key(), *expected);
    }
  }
}

// One writer, several readers, no locks. Each reader repeatedly scans and
// checks that what it sees is sorted and was actually inserted. Run under
// ThreadSanitizer this also checks the memory ordering, not just the result.
TEST(SkipList, ConcurrentReadersSeeConsistentState) {
  constexpr uint64_t kKeys = 20000;
  constexpr int kReaders = 3;

  Arena arena;
  TestList list(U64Comparator(), &arena);
  std::atomic<uint64_t> written{0};
  std::atomic<bool> done{false};
  std::atomic<int> scans{0};
  std::atomic<bool> failed{false};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      while (!done.load(std::memory_order_acquire)) {
        // Everything below this watermark is guaranteed present; entries
        // above it may or may not have been linked in yet.
        const uint64_t watermark = written.load(std::memory_order_acquire);
        uint64_t last = 0;
        bool first = true;
        uint64_t seen_below_watermark = 0;
        TestList::Iterator iter(&list);
        for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
          const uint64_t key = iter.key();
          if (!first && key <= last) failed.store(true);  // out of order
          if (key >= kKeys) failed.store(true);           // never inserted
          if (key < watermark) ++seen_below_watermark;
          last = key;
          first = false;
        }
        if (seen_below_watermark < watermark) failed.store(true);  // lost a write
        scans.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (uint64_t i = 0; i < kKeys; ++i) {
    list.Insert(i);
    written.store(i + 1, std::memory_order_release);
  }
  done.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();

  CHECK(!failed.load());
  CHECK(scans.load() > 0);
  CHECK_EQ(list.ApproximateCount(), static_cast<size_t>(kKeys));
  std::printf("       (%d concurrent scans over %llu keys)\n", scans.load(),
              static_cast<unsigned long long>(kKeys));
}

TEST(DbFormat, InternalKeysSortByUserKeyThenNewestFirst) {
  // Same user key: the higher sequence sorts first, so a seek finds the
  // newest version.
  CHECK_LT(CompareInternalKey(Ikey("a", 9, ValueType::kValue), Ikey("a", 5, ValueType::kValue)),
           0);
  // Different user keys: byte order wins regardless of sequence.
  CHECK_LT(CompareInternalKey(Ikey("a", 1, ValueType::kValue), Ikey("b", 99, ValueType::kValue)),
           0);
  // A value and a tombstone at the same sequence are ordered by type, and a
  // key equals itself.
  CHECK_EQ(CompareInternalKey(Ikey("a", 5, ValueType::kValue), Ikey("a", 5, ValueType::kValue)),
           0);
  CHECK_LT(CompareInternalKey(Ikey("a", 5, ValueType::kValue), Ikey("a", 5, ValueType::kDeletion)),
           0);
}

TEST(DbFormat, ParseRoundTripsAndRejectsGarbage) {
  const std::string encoded = Ikey("hello", 1234, ValueType::kDeletion);
  ParsedInternalKey parsed;
  CHECK(ParseInternalKey(encoded, &parsed));
  CHECK_EQ(parsed.user_key, std::string_view("hello"));
  CHECK_EQ(parsed.sequence, 1234u);
  CHECK(parsed.type == ValueType::kDeletion);

  CHECK(!ParseInternalKey("short", &parsed));  // no room for a tag
  std::string bad_type = encoded;
  bad_type[bad_type.size() - 8] = static_cast<char>(0x7f);
  CHECK(!ParseInternalKey(bad_type, &parsed));

  CHECK_EQ(ExtractUserKey(encoded), std::string_view("hello"));
  CHECK_EQ(ExtractSequence(encoded), 1234u);
}

TEST(DbFormat, SeparatorsStayBetweenTheKeys) {
  struct Pair {
    const char* start;
    const char* limit;
  };
  const Pair pairs[] = {
      {"abc", "abd"}, {"abc", "xyz"}, {"abc", "abcde"}, {"", "a"}, {"a", "a"}, {"\xff\xff", "\xff\xff\xff"},
  };
  for (const auto& p : pairs) {
    const std::string sep = ShortestSeparator(p.start, p.limit);
    // A separator must be usable as an index entry: >= start, and < limit
    // whenever start < limit.
    CHECK(sep >= std::string(p.start));
    if (std::string(p.start) < std::string(p.limit)) CHECK(sep < std::string(p.limit));
    CHECK(sep.size() <= std::string(p.start).size());
  }
}

TEST(DbFormat, SuccessorIsGreater) {
  for (const char* key : {"abc", "", "a\xff", "\xff\xff\xff"}) {
    const std::string succ = ShortSuccessor(key);
    const std::string original(key);
    if (original.find_first_not_of('\xff') != std::string::npos || original.empty()) {
      CHECK(succ >= original);
    }
  }
  CHECK_EQ(ShortSuccessor("abc"), std::string("b"));
  CHECK_EQ(ShortSuccessor("a\xff\xff"), std::string("b"));
}

// Short keys use the inline buffer, long ones spill to the heap. Both must
// produce identical encodings.
TEST(DbFormat, LookupKeyMatchesInBothStoragePaths) {
  for (size_t len : {size_t{0}, size_t{1}, size_t{100}, size_t{186}, size_t{187}, size_t{188},
                     size_t{5000}}) {
    const std::string user_key(len, 'k');
    const LookupKey lk(user_key, 999);
    CHECK_EQ(lk.user_key(), std::string_view(user_key));
    CHECK_EQ(lk.internal_key().size(), len + 8);
    CHECK_EQ(ExtractSequence(lk.internal_key()), 999u);
    // The memtable key is the internal key with a varint length in front.
    CHECK(lk.memtable_key().size() > lk.internal_key().size());
    CHECK(lk.memtable_key().ends_with(lk.internal_key()));
  }
}

TEST(MemTable, ReadsBackWhatWasWritten) {
  MemTable table;
  table.Add(1, ValueType::kValue, "alpha", "one");
  table.Add(2, ValueType::kValue, "beta", "two");
  table.Add(3, ValueType::kValue, "gamma", "");

  std::string value;
  Status s;
  CHECK(table.Get(LookupKey("alpha", 10), &value, &s));
  CHECK_OK(s);
  CHECK_EQ(value, std::string("one"));

  CHECK(table.Get(LookupKey("gamma", 10), &value, &s));
  CHECK_OK(s);
  CHECK_EQ(value, std::string(""));  // an empty value is a value

  CHECK(!table.Get(LookupKey("delta", 10), &value, &s));  // absent, not deleted
  CHECK_EQ(table.EntryCount(), 3u);
}

TEST(MemTable, NewestSequenceWins) {
  MemTable table;
  table.Add(1, ValueType::kValue, "k", "first");
  table.Add(2, ValueType::kValue, "k", "second");
  table.Add(3, ValueType::kValue, "k", "third");

  std::string value;
  Status s;
  CHECK(table.Get(LookupKey("k", 100), &value, &s));
  CHECK_EQ(value, std::string("third"));

  // A snapshot read must not see writes made after it.
  CHECK(table.Get(LookupKey("k", 2), &value, &s));
  CHECK_EQ(value, std::string("second"));
  CHECK(table.Get(LookupKey("k", 1), &value, &s));
  CHECK_EQ(value, std::string("first"));
  CHECK(!table.Get(LookupKey("k", 0), &value, &s));  // before the first write
}

TEST(MemTable, TombstoneStopsTheSearch) {
  MemTable table;
  table.Add(1, ValueType::kValue, "k", "value");
  table.Add(2, ValueType::kDeletion, "k", "");

  std::string value;
  Status s;
  // Found an answer (true) and the answer is "deleted" (NotFound). Returning
  // false here would let an older table serve the stale value.
  CHECK(table.Get(LookupKey("k", 100), &value, &s));
  CHECK(s.IsNotFound());

  CHECK(table.Get(LookupKey("k", 1), &value, &s));
  CHECK_OK(s);
  CHECK_EQ(value, std::string("value"));
}

TEST(MemTable, IteratesInInternalKeyOrder) {
  MemTable table;
  table.Add(1, ValueType::kValue, "b", "1");
  table.Add(2, ValueType::kValue, "a", "2");
  table.Add(3, ValueType::kValue, "b", "3");
  table.Add(4, ValueType::kDeletion, "c", "");

  auto iter = table.NewIterator();
  std::vector<std::string> seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ParsedInternalKey parsed;
    CHECK(ParseInternalKey(iter->key(), &parsed));
    seen.push_back(std::string(parsed.user_key) + "@" + std::to_string(parsed.sequence) + "=" +
                   std::string(iter->value()));
  }
  const std::vector<std::string> expected = {"a@2=2", "b@3=3", "b@1=1", "c@4="};
  CHECK_EQ(seen.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) CHECK_EQ(seen[i], expected[i]);
}

TEST(MemTable, SeekAndReverseIteration) {
  MemTable table;
  for (int i = 0; i < 100; ++i) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%03d", i);
    table.Add(static_cast<SequenceNumber>(i + 1), ValueType::kValue, key, std::to_string(i));
  }
  auto iter = table.NewIterator();

  std::string target;
  AppendInternalKey(&target, {"key050", kMaxSequenceNumber, kValueTypeForSeek});
  iter->Seek(target);
  CHECK(iter->Valid());
  CHECK_EQ(ExtractUserKey(iter->key()), std::string_view("key050"));

  int count = 0;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) ++count;
  CHECK_EQ(count, 100);
}

TEST(MemTable, HandlesLargeValuesAndManyKeys) {
  MemTable table;
  const std::string big(100000, 'v');
  table.Add(1, ValueType::kValue, "big", big);

  std::mt19937 rng(42);
  std::map<std::string, std::string> model;
  for (int i = 0; i < 5000; ++i) {
    const std::string key = "k" + std::to_string(rng() % 2000);
    const std::string value = std::to_string(i);
    table.Add(static_cast<SequenceNumber>(i + 2), ValueType::kValue, key, value);
    model[key] = value;  // later writes win, as in the memtable
  }

  std::string value;
  Status s;
  CHECK(table.Get(LookupKey("big", kMaxSequenceNumber), &value, &s));
  CHECK_EQ(value.size(), big.size());
  for (const auto& [k, v] : model) {
    CHECK(table.Get(LookupKey(k, kMaxSequenceNumber), &value, &s));
    CHECK_OK(s);
    CHECK_EQ(value, v);
  }
  CHECK(table.ApproximateMemoryUsage() > big.size());
}

}  // namespace lsmkv
