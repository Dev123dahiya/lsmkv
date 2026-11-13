#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/dbformat.h"
#include "harness.h"
#include "lsmkv/db.h"
#include "testutil.h"

namespace lsmkv {
namespace {

Options TestOptions() {
  Options options;
  // Small enough that a few thousand writes produce real flushes and
  // compactions rather than sitting in one memtable.
  options.write_buffer_size = 64 * 1024;
  options.max_file_size = 64 * 1024;
  options.block_size = 1024;
  options.level0_compaction_trigger = 3;
  options.level0_slowdown_trigger = 6;
  options.level0_stop_trigger = 10;
  return options;
}

std::unique_ptr<DB> OpenAt(const std::string& path, const Options& options) {
  std::unique_ptr<DB> db;
  const Status s = DB::Open(options, path, &db);
  if (!s.ok()) {
    std::printf("       open failed: %s\n", s.ToString().c_str());
    return nullptr;
  }
  return db;
}

std::string Get(DB* db, std::string_view key, const ReadOptions& options = ReadOptions()) {
  std::string value;
  const Status s = db->Get(options, key, &value);
  if (s.IsNotFound()) return "<absent>";
  if (!s.ok()) return "<error: " + s.ToString() + ">";
  return value;
}

}  // namespace

TEST(DB, PutGetDelete) {
  test::ScopedTempDir dir("basic");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  CHECK_EQ(Get(db.get(), "missing"), std::string("<absent>"));

  CHECK_OK(db->Put(WriteOptions(), "a", "one"));
  CHECK_OK(db->Put(WriteOptions(), "b", "two"));
  CHECK_EQ(Get(db.get(), "a"), std::string("one"));
  CHECK_EQ(Get(db.get(), "b"), std::string("two"));

  CHECK_OK(db->Put(WriteOptions(), "a", "overwritten"));
  CHECK_EQ(Get(db.get(), "a"), std::string("overwritten"));

  CHECK_OK(db->Delete(WriteOptions(), "a"));
  CHECK_EQ(Get(db.get(), "a"), std::string("<absent>"));
  CHECK_EQ(Get(db.get(), "b"), std::string("two"));

  // Deleting a key that was never there is not an error.
  CHECK_OK(db->Delete(WriteOptions(), "never-existed"));

  // An empty value is a value, not an absence.
  CHECK_OK(db->Put(WriteOptions(), "empty", ""));
  CHECK_EQ(Get(db.get(), "empty"), std::string(""));
}

TEST(DB, WriteBatchIsAtomicAndOrdered) {
  test::ScopedTempDir dir("batch");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  WriteBatch batch;
  batch.Put("k1", "v1");
  batch.Put("k2", "v2");
  batch.Delete("k1");
  batch.Put("k3", "v3");
  CHECK_EQ(batch.Count(), 4u);
  CHECK_OK(db->Write(WriteOptions(), &batch));

  // Later records in a batch win over earlier ones for the same key.
  CHECK_EQ(Get(db.get(), "k1"), std::string("<absent>"));
  CHECK_EQ(Get(db.get(), "k2"), std::string("v2"));
  CHECK_EQ(Get(db.get(), "k3"), std::string("v3"));

  WriteBatch second;
  second.Put("k1", "back");
  WriteBatch combined;
  combined.Put("k4", "v4");
  combined.Append(second);
  CHECK_EQ(combined.Count(), 2u);
  CHECK_OK(db->Write(WriteOptions(), &combined));
  CHECK_EQ(Get(db.get(), "k1"), std::string("back"));
  CHECK_EQ(Get(db.get(), "k4"), std::string("v4"));
}

TEST(DB, DataSurvivesReopen) {
  test::ScopedTempDir dir("reopen");
  const Options options = TestOptions();

  {
    auto db = OpenAt(dir.path(), options);
    CHECK(db != nullptr);
    for (int i = 0; i < 2000; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                       "value-" + std::to_string(i)));
    }
    for (int i = 0; i < 2000; i += 3) {
      CHECK_OK(db->Delete(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i))));
    }
  }

  // A clean close, then a fresh open: every acknowledged write must be here.
  {
    auto db = OpenAt(dir.path(), options);
    CHECK(db != nullptr);
    int present = 0, absent = 0;
    for (int i = 0; i < 2000; ++i) {
      const std::string got = Get(db.get(), test::NumericKey(static_cast<uint64_t>(i)));
      if (i % 3 == 0) {
        CHECK_EQ(got, std::string("<absent>"));
        ++absent;
      } else {
        CHECK_EQ(got, "value-" + std::to_string(i));
        ++present;
      }
    }
    std::printf("       (%d values and %d tombstones survived a reopen)\n", present, absent);
  }
}

TEST(DB, SnapshotsSeeThePastNotTheFuture) {
  test::ScopedTempDir dir("snapshot");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  CHECK_OK(db->Put(WriteOptions(), "k", "first"));
  const Snapshot* early = db->GetSnapshot();

  CHECK_OK(db->Put(WriteOptions(), "k", "second"));
  CHECK_OK(db->Put(WriteOptions(), "added-later", "x"));
  const Snapshot* late = db->GetSnapshot();

  CHECK_OK(db->Put(WriteOptions(), "k", "third"));
  CHECK_OK(db->Delete(WriteOptions(), "added-later"));

  ReadOptions at_early;
  at_early.snapshot = early;
  ReadOptions at_late;
  at_late.snapshot = late;

  CHECK_EQ(Get(db.get(), "k", at_early), std::string("first"));
  CHECK_EQ(Get(db.get(), "k", at_late), std::string("second"));
  CHECK_EQ(Get(db.get(), "k"), std::string("third"));

  // A key created after the early snapshot is invisible to it; one deleted
  // after the late snapshot is still visible to that one.
  CHECK_EQ(Get(db.get(), "added-later", at_early), std::string("<absent>"));
  CHECK_EQ(Get(db.get(), "added-later", at_late), std::string("x"));
  CHECK_EQ(Get(db.get(), "added-later"), std::string("<absent>"));

  db->ReleaseSnapshot(early);
  db->ReleaseSnapshot(late);
  CHECK_EQ(Get(db.get(), "k"), std::string("third"));
}

// A snapshot has to survive the compaction that would otherwise drop the
// versions it needs.
TEST(DB, SnapshotSurvivesCompaction) {
  test::ScopedTempDir dir("snapcompact");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  for (int i = 0; i < 200; ++i) {
    CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)), "original"));
  }
  const Snapshot* snapshot = db->GetSnapshot();

  // Overwrite everything several times and force the merges through.
  for (int round = 0; round < 8; ++round) {
    for (int i = 0; i < 200; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                       "round-" + std::to_string(round)));
    }
  }
  CHECK_OK(db->CompactRange("", ""));
  db->WaitForCompaction();

  ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot;
  for (int i = 0; i < 200; ++i) {
    CHECK_EQ(Get(db.get(), test::NumericKey(static_cast<uint64_t>(i)), at_snapshot),
             std::string("original"));
    CHECK_EQ(Get(db.get(), test::NumericKey(static_cast<uint64_t>(i))), std::string("round-7"));
  }
  db->ReleaseSnapshot(snapshot);
}

TEST(DB, IteratesInOrderBothWays) {
  test::ScopedTempDir dir("iterate");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  std::map<std::string, std::string> model;
  for (int i = 0; i < 1500; ++i) {
    const std::string key = test::NumericKey(static_cast<uint64_t>(i));
    const std::string value = "value-" + std::to_string(i);
    CHECK_OK(db->Put(WriteOptions(), key, value));
    model[key] = value;
  }
  // Delete a third of them, which leaves tombstones for the iterator to hide.
  for (int i = 0; i < 1500; i += 3) {
    const std::string key = test::NumericKey(static_cast<uint64_t>(i));
    CHECK_OK(db->Delete(WriteOptions(), key));
    model.erase(key);
  }

  auto iter = db->NewIterator(ReadOptions());

  auto expected = model.begin();
  int forwards = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    CHECK(expected != model.end());
    CHECK_EQ(iter->key(), std::string_view(expected->first));
    CHECK_EQ(iter->value(), std::string_view(expected->second));
    ++expected;
    ++forwards;
  }
  CHECK_OK(iter->status());
  CHECK(expected == model.end());
  CHECK_EQ(forwards, static_cast<int>(model.size()));

  auto backward = model.rbegin();
  int backwards = 0;
  for (iter->SeekToLast(); iter->Valid(); iter->Prev()) {
    CHECK(backward != model.rend());
    CHECK_EQ(iter->key(), std::string_view(backward->first));
    CHECK_EQ(iter->value(), std::string_view(backward->second));
    ++backward;
    ++backwards;
  }
  CHECK_OK(iter->status());
  CHECK_EQ(backwards, forwards);
}

TEST(DB, SeekLandsOnTheRightKey) {
  test::ScopedTempDir dir("seek");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  std::map<std::string, std::string> model;
  for (int i = 0; i < 600; ++i) {
    const std::string key = test::NumericKey(static_cast<uint64_t>(i * 2));  // even only
    CHECK_OK(db->Put(WriteOptions(), key, "v"));
    model[key] = "v";
  }

  auto iter = db->NewIterator(ReadOptions());
  for (int i = 0; i < 1200; ++i) {
    const std::string target = test::NumericKey(static_cast<uint64_t>(i));
    iter->Seek(target);
    const auto expected = model.lower_bound(target);
    if (expected == model.end()) {
      CHECK(!iter->Valid());
    } else {
      CHECK(iter->Valid());
      CHECK_EQ(iter->key(), std::string_view(expected->first));
    }
  }
  CHECK_OK(iter->status());
}

// The main correctness test: thousands of random operations against the
// database and a std::map, checking every read, scan and reopen agrees.
TEST(DB, MatchesAnOrderedMapUnderRandomOperations) {
  constexpr int kOperations = 20000;
  constexpr int kKeySpace = 700;

  test::ScopedTempDir dir("differential");
  const Options options = TestOptions();
  auto db = OpenAt(dir.path(), options);
  CHECK(db != nullptr);

  std::map<std::string, std::string> model;
  std::mt19937 rng(20261113);
  int reopens = 0, puts = 0, deletes = 0, batches = 0;

  for (int op = 0; op < kOperations; ++op) {
    const int choice = static_cast<int>(rng() % 100);
    const std::string key = test::NumericKey(rng() % kKeySpace);

    if (choice < 55) {
      // Values of wildly different sizes, so blocks fill unevenly.
      const size_t len = (rng() % 20 == 0) ? 2000 + rng() % 4000 : rng() % 60;
      const std::string value = test::RandomString(rng, len);
      CHECK_OK(db->Put(WriteOptions(), key, value));
      model[key] = value;
      ++puts;
    } else if (choice < 70) {
      CHECK_OK(db->Delete(WriteOptions(), key));
      model.erase(key);
      ++deletes;
    } else if (choice < 85) {
      // A batch mixing puts and deletes, applied atomically.
      WriteBatch batch;
      std::map<std::string, std::string> pending_put;
      std::set<std::string> pending_delete;
      const int n = 1 + static_cast<int>(rng() % 8);
      for (int j = 0; j < n; ++j) {
        const std::string k = test::NumericKey(rng() % kKeySpace);
        if (rng() % 4 == 0) {
          batch.Delete(k);
          pending_delete.insert(k);
          pending_put.erase(k);
        } else {
          const std::string v = test::RandomString(rng, rng() % 40);
          batch.Put(k, v);
          pending_put[k] = v;
          pending_delete.erase(k);
        }
      }
      CHECK_OK(db->Write(WriteOptions(), &batch));
      for (const auto& k : pending_delete) model.erase(k);
      for (const auto& [k, v] : pending_put) model[k] = v;
      ++batches;
    } else if (choice < 98) {
      // Point read.
      const std::string got = Get(db.get(), key);
      const auto expected = model.find(key);
      if (expected == model.end()) {
        CHECK_EQ(got, std::string("<absent>"));
      } else {
        CHECK_EQ(got, expected->second);
      }
    } else {
      // Close and reopen: everything acknowledged must come back.
      db.reset();
      db = OpenAt(dir.path(), options);
      CHECK(db != nullptr);
      ++reopens;
    }
  }

  // Full scan must match the model exactly.
  auto iter = db->NewIterator(ReadOptions());
  auto expected = model.begin();
  size_t scanned = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    CHECK(expected != model.end());
    CHECK_EQ(iter->key(), std::string_view(expected->first));
    CHECK_EQ(iter->value(), std::string_view(expected->second));
    ++expected;
    ++scanned;
  }
  CHECK_OK(iter->status());
  CHECK(expected == model.end());
  CHECK_EQ(scanned, model.size());

  // And every key individually.
  for (const auto& [k, v] : model) CHECK_EQ(Get(db.get(), k), v);

  std::printf("       (%d puts, %d deletes, %d batches, %d reopens; %zu keys survive)\n", puts,
              deletes, batches, reopens, model.size());
}

TEST(DB, CompactionBuildsLevelsAndDropsOverwrittenKeys) {
  test::ScopedTempDir dir("compaction");
  Options options = TestOptions();
  auto db = OpenAt(dir.path(), options);
  CHECK(db != nullptr);

  // Overwrite a small key space many times: almost everything written is
  // garbage that compaction should reclaim.
  constexpr int kKeys = 400;
  constexpr int kRounds = 30;
  for (int round = 0; round < kRounds; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                       std::string(200, static_cast<char>('a' + round % 26))));
    }
  }
  CHECK_OK(db->CompactRange("", ""));
  db->WaitForCompaction();

  // Every key still reads back as its last write.
  const std::string expected(200, static_cast<char>('a' + (kRounds - 1) % 26));
  for (int i = 0; i < kKeys; ++i) {
    CHECK_EQ(Get(db.get(), test::NumericKey(static_cast<uint64_t>(i))), expected);
  }

  const std::string stats = db->Stats();
  CHECK(stats.find("level") != std::string::npos);
  CHECK(stats.find("compactions") != std::string::npos);
  std::printf("%s", stats.c_str());
}

TEST(DB, DeletedKeysDoNotComeBackAfterCompaction) {
  test::ScopedTempDir dir("tombstones");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  for (int i = 0; i < 1000; ++i) {
    CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                     std::string(300, 'v')));
  }
  CHECK_OK(db->CompactRange("", ""));

  // Delete half, then write enough to push the tombstones down through the
  // levels where the older values live.
  for (int i = 0; i < 1000; i += 2) {
    CHECK_OK(db->Delete(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i))));
  }
  for (int i = 2000; i < 3000; ++i) {
    CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                     std::string(300, 'w')));
  }
  CHECK_OK(db->CompactRange("", ""));
  db->WaitForCompaction();

  for (int i = 0; i < 1000; ++i) {
    const std::string got = Get(db.get(), test::NumericKey(static_cast<uint64_t>(i)));
    if (i % 2 == 0) {
      CHECK_EQ(got, std::string("<absent>"));
    } else {
      CHECK_EQ(got.size(), 300u);
    }
  }
}

TEST(DB, ConcurrentReadersAndOneWriter) {
  test::ScopedTempDir dir("concurrent");
  auto db = OpenAt(dir.path(), TestOptions());
  CHECK(db != nullptr);

  constexpr int kKeys = 500;
  for (int i = 0; i < kKeys; ++i) {
    CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)), "initial"));
  }

  std::atomic<bool> done{false};
  std::atomic<bool> failed{false};
  std::atomic<int> reads{0};

  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    // `t` is captured by value: the loop variable goes out of scope long
    // before the thread stops reading it.
    readers.emplace_back([&, t] {
      std::mt19937 rng(static_cast<uint32_t>(t + 1));
      while (!done.load(std::memory_order_acquire)) {
        // Every key is written before the readers start and never deleted,
        // so a read must always find something.
        const std::string key = test::NumericKey(rng() % kKeys);
        std::string value;
        const Status s = db->Get(ReadOptions(), key, &value);
        if (!s.ok() || value.empty()) failed.store(true);
        reads.fetch_add(1, std::memory_order_relaxed);

        // And a scan must stay in order throughout.
        auto iter = db->NewIterator(ReadOptions());
        std::string previous;
        int seen = 0;
        for (iter->SeekToFirst(); iter->Valid() && seen < 50; iter->Next(), ++seen) {
          if (!previous.empty() && std::string(iter->key()) <= previous) failed.store(true);
          previous.assign(iter->key());
        }
      }
    });
  }

  for (int round = 0; round < 25; ++round) {
    for (int i = 0; i < kKeys; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                       "round-" + std::to_string(round)));
    }
  }
  done.store(true, std::memory_order_release);
  for (auto& t : readers) t.join();

  CHECK(!failed.load());
  CHECK(reads.load() > 0);
  for (int i = 0; i < kKeys; ++i) {
    CHECK_EQ(Get(db.get(), test::NumericKey(static_cast<uint64_t>(i))), std::string("round-24"));
  }
  std::printf("       (%d reads from 3 threads during %d writes)\n", reads.load(), 25 * kKeys);
}

TEST(DB, RejectsOpeningTwice) {
  test::ScopedTempDir dir("locked");
  auto first = OpenAt(dir.path(), TestOptions());
  CHECK(first != nullptr);

  // The lock file is what stops two processes corrupting one database.
  std::unique_ptr<DB> second;
  const Status s = DB::Open(TestOptions(), dir.path(), &second);
  CHECK(!s.ok());
  CHECK(s.IsIOError());
}

TEST(DB, HonoursCreateAndExistsOptions) {
  test::ScopedTempDir dir("flags");
  const std::string path = dir.path() + "/sub";

  Options no_create = TestOptions();
  no_create.create_if_missing = false;
  std::unique_ptr<DB> missing;
  CHECK(DB::Open(no_create, path, &missing).IsNotFound());

  {
    auto db = OpenAt(path, TestOptions());
    CHECK(db != nullptr);
    CHECK_OK(db->Put(WriteOptions(), "k", "v"));
  }

  Options must_not_exist = TestOptions();
  must_not_exist.error_if_exists = true;
  std::unique_ptr<DB> again;
  CHECK(!DB::Open(must_not_exist, path, &again).ok());

  DestroyDB(path, Options());
}

}  // namespace lsmkv
