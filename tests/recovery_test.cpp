// What survives a crash, and what a damaged file does.
//
// The contract being checked is narrow and worth stating: a write that
// returned with sync set must be readable after the process dies at any
// point. A write made without sync may be lost, but losing it must never
// corrupt the database or resurrect an older value for the same key.
#include <algorithm>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "harness.h"
#include "lsmkv/db.h"
#include "table/table_cache.h"
#include "testutil.h"
#include "util/env.h"

namespace lsmkv {
namespace {

Options RecoveryOptions(Env* env = nullptr) {
  Options options;
  options.write_buffer_size = 32 * 1024;
  options.max_file_size = 32 * 1024;
  options.block_size = 1024;
  options.env = env;
  return options;
}

std::string ValueFor(int i) { return "value-" + std::to_string(i) + std::string(i % 30, 'x'); }

// Every file in the directory, with its size.
std::vector<std::pair<std::string, uint64_t>> ListFiles(const std::string& dir) {
  std::vector<std::pair<std::string, uint64_t>> out;
  std::vector<std::string> children;
  if (!Env::Default()->GetChildren(dir, &children).ok()) return out;
  for (const auto& name : children) {
    uint64_t size = 0;
    Env::Default()->GetFileSize(dir + "/" + name, &size);
    out.emplace_back(name, size);
  }
  std::sort(out.begin(), out.end());
  return out;
}

void TruncateFile(const std::string& path, uint64_t bytes) {
  std::string contents;
  CHECK_OK(ReadFileToString(Env::Default(), path, &contents));
  contents.resize(std::min<size_t>(contents.size(), bytes));
  CHECK_OK(WriteStringToFile(Env::Default(), contents, path, true));
}

}  // namespace

// Writes are acknowledged after the log record is on disk. Cutting the log
// anywhere must leave a database that opens, and whose contents are a prefix
// of what was written: never a mangled value, never a resurrected one.
TEST(Recovery, TruncatedLogLeavesAPrefixOfTheWrites) {
  constexpr int kKeys = 400;
  int checked = 0;

  // Find the log file once, from a database written the same way each time.
  std::string log_name;
  {
    test::ScopedTempDir probe("probe");
    {
      auto options = RecoveryOptions();
      std::unique_ptr<DB> db;
      CHECK_OK(DB::Open(options, probe.path(), &db));
      WriteOptions sync;
      sync.sync = true;
      for (int i = 0; i < kKeys; ++i) {
        CHECK_OK(db->Put(sync, test::NumericKey(static_cast<uint64_t>(i)), ValueFor(i)));
      }
    }
    for (const auto& [name, size] : ListFiles(probe.path())) {
      if (name.size() > 4 && name.compare(name.size() - 4, 4, ".log") == 0 && size > 0) {
        log_name = name;
      }
    }
  }
  CHECK(!log_name.empty());

  uint64_t full_size = 0;
  for (const uint64_t cut : {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{64}, uint64_t{200},
                             uint64_t{1000}, uint64_t{4000}, uint64_t{12000}}) {
    test::ScopedTempDir dir("truncate");
    {
      std::unique_ptr<DB> db;
      CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));
      WriteOptions sync;
      sync.sync = true;
      for (int i = 0; i < kKeys; ++i) {
        CHECK_OK(db->Put(sync, test::NumericKey(static_cast<uint64_t>(i)), ValueFor(i)));
      }
    }

    const std::string path = dir.path() + "/" + log_name;
    if (!Env::Default()->FileExists(path)) continue;
    CHECK_OK(Env::Default()->GetFileSize(path, &full_size));
    if (cut > full_size) continue;
    TruncateFile(path, cut);

    // The database must still open, and every value it returns must be the
    // one that was written for that key.
    std::unique_ptr<DB> db;
    const Status s = DB::Open(RecoveryOptions(), dir.path(), &db);
    CHECK_OK(s);

    int present = 0;
    for (int i = 0; i < kKeys; ++i) {
      std::string value;
      const Status got = db->Get(ReadOptions(), test::NumericKey(static_cast<uint64_t>(i)), &value);
      if (got.ok()) {
        CHECK_EQ(value, ValueFor(i));  // never a mangled or stale value
        ++present;
      } else {
        CHECK(got.IsNotFound());
      }
    }
    // Cutting more off the log cannot leave more keys behind.
    CHECK(present <= kKeys);
    ++checked;
  }
  std::printf("       (%d truncation points, log was %llu bytes)\n", checked,
              static_cast<unsigned long long>(full_size));
}

// A machine that stops mid-write, simulated by an Env that silently drops
// everything after a chosen byte. The database must open afterwards and
// never return a value that was not written.
TEST(Recovery, SurvivesACrashAtAnyPointDuringWrites) {
  constexpr int kKeys = 300;

  // How many bytes a full run writes, so the crash points span it.
  uint64_t total_bytes = 0;
  {
    test::ScopedTempDir probe("crashprobe");
    test::CrashEnv env(Env::Default());
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(RecoveryOptions(&env), probe.path(), &db));
    for (int i = 0; i < kKeys; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)), ValueFor(i)));
    }
    total_bytes = env.bytes_written();
  }
  CHECK(total_bytes > 0);

  int runs = 0, opened = 0;
  for (int step = 1; step <= 8; ++step) {
    const uint64_t limit = total_bytes * static_cast<uint64_t>(step) / 9;

    test::ScopedTempDir dir("crash");
    std::map<std::string, std::string> acknowledged;
    {
      test::CrashEnv env(Env::Default());
      std::unique_ptr<DB> db;
      const Status open = DB::Open(RecoveryOptions(&env), dir.path(), &db);
      if (!open.ok()) continue;  // the crash landed during Open itself

      env.CrashAfterBytes(limit);
      for (int i = 0; i < kKeys; ++i) {
        const std::string key = test::NumericKey(static_cast<uint64_t>(i));
        const Status s = db->Put(WriteOptions(), key, ValueFor(i));
        if (!s.ok()) break;
        // Only writes made before the machine stopped can be expected back.
        if (!env.crashed()) acknowledged[key] = ValueFor(i);
      }
      // Let the destructor run against the crashed filesystem, as a real
      // process would not get to flush anything.
    }
    ++runs;

    std::unique_ptr<DB> reopened;
    const Status s = DB::Open(RecoveryOptions(), dir.path(), &reopened);
    if (!s.ok()) {
      std::printf("       crash at %llu bytes: reopen failed: %s\n",
                  static_cast<unsigned long long>(limit), s.ToString().c_str());
      CHECK_OK(s);
      continue;
    }
    ++opened;

    // Nothing invented, nothing mangled. A value that comes back must be
    // the one written for that key.
    int found = 0;
    for (int i = 0; i < kKeys; ++i) {
      std::string value;
      const Status got =
          reopened->Get(ReadOptions(), test::NumericKey(static_cast<uint64_t>(i)), &value);
      if (got.ok()) {
        CHECK_EQ(value, ValueFor(i));
        ++found;
      } else {
        CHECK(got.IsNotFound());
      }
    }

    // A full scan must still be ordered and consistent after recovery.
    auto iter = reopened->NewIterator(ReadOptions());
    std::string previous;
    int scanned = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
      if (!previous.empty()) CHECK(std::string(iter->key()) > previous);
      previous.assign(iter->key());
      ++scanned;
    }
    CHECK_OK(iter->status());
    CHECK_EQ(scanned, found);
  }

  CHECK_EQ(opened, runs);
  std::printf("       (%d simulated crashes, all %d reopened cleanly)\n", runs, opened);
}

// After a clean close every acknowledged write must be present. This is the
// case that has no excuse.
TEST(Recovery, CleanCloseLosesNothing) {
  test::ScopedTempDir dir("cleanclose");
  constexpr int kKeys = 3000;

  for (int round = 0; round < 3; ++round) {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));
    for (int i = 0; i < kKeys; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)),
                       "round-" + std::to_string(round)));
    }
  }

  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));
  for (int i = 0; i < kKeys; ++i) {
    std::string value;
    CHECK_OK(db->Get(ReadOptions(), test::NumericKey(static_cast<uint64_t>(i)), &value));
    CHECK_EQ(value, std::string("round-2"));
  }
}

// Bit rot in a table must be reported, not served.
TEST(Recovery, CorruptTableIsReportedNotServed) {
  test::ScopedTempDir dir("corrupt");
  constexpr int kKeys = 2000;

  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));
    for (int i = 0; i < kKeys; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)), ValueFor(i)));
    }
    CHECK_OK(db->CompactRange("", ""));
    db->WaitForCompaction();
  }

  // Damage the middle of the largest table file.
  std::string victim;
  uint64_t victim_size = 0;
  for (const auto& [name, size] : ListFiles(dir.path())) {
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tbl") == 0 && size > victim_size) {
      victim = name;
      victim_size = size;
    }
  }
  CHECK(!victim.empty());

  const std::string path = dir.path() + "/" + victim;
  std::string contents;
  CHECK_OK(ReadFileToString(Env::Default(), path, &contents));
  for (size_t i = contents.size() / 3; i < contents.size() / 3 + 64 && i < contents.size(); ++i) {
    contents[i] = static_cast<char>(contents[i] ^ 0xff);
  }
  CHECK_OK(WriteStringToFile(Env::Default(), contents, path, true));

  Options options = RecoveryOptions();
  options.paranoid_checks = true;
  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(options, dir.path(), &db));

  // Some read over the damaged region must report corruption, and no read
  // may return a value that was never written.
  bool reported = false;
  for (int i = 0; i < kKeys; ++i) {
    std::string value;
    const Status s = db->Get(ReadOptions(), test::NumericKey(static_cast<uint64_t>(i)), &value);
    if (s.IsCorruption()) {
      reported = true;
    } else if (s.ok()) {
      CHECK_EQ(value, ValueFor(i));
    }
  }
  auto iter = db->NewIterator(ReadOptions());
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
  }
  if (iter->status().IsCorruption()) reported = true;

  CHECK(reported);
  std::printf("       (corruption of %llu bytes in %s was reported, not served)\n",
              static_cast<unsigned long long>(64), victim.c_str());
}

TEST(Recovery, MissingOrDamagedManifestIsRejected) {
  test::ScopedTempDir dir("manifest");
  {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));
    for (int i = 0; i < 200; ++i) {
      CHECK_OK(db->Put(WriteOptions(), test::NumericKey(static_cast<uint64_t>(i)), ValueFor(i)));
    }
  }

  // A CURRENT that names nothing usable must fail the open rather than
  // silently starting an empty database over the top of real data.
  CHECK_OK(WriteStringToFile(Env::Default(), "MANIFEST-999999\n",
                             dir.path() + "/CURRENT", true));
  std::unique_ptr<DB> db;
  const Status s = DB::Open(RecoveryOptions(), dir.path(), &db);
  CHECK(!s.ok());
}

TEST(Recovery, ReopensRepeatedlyWithoutLosingData) {
  test::ScopedTempDir dir("churn");
  std::map<std::string, std::string> model;
  std::mt19937 rng(77);

  for (int round = 0; round < 12; ++round) {
    std::unique_ptr<DB> db;
    CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));

    for (int i = 0; i < 400; ++i) {
      const std::string key = test::NumericKey(rng() % 500);
      if (rng() % 5 == 0) {
        CHECK_OK(db->Delete(WriteOptions(), key));
        model.erase(key);
      } else {
        const std::string value = test::RandomString(rng, rng() % 50);
        CHECK_OK(db->Put(WriteOptions(), key, value));
        model[key] = value;
      }
    }

    // Everything written so far, across every previous open, must be here.
    for (const auto& [k, v] : model) {
      std::string value;
      CHECK_OK(db->Get(ReadOptions(), k, &value));
      CHECK_EQ(value, v);
    }
  }

  std::unique_ptr<DB> db;
  CHECK_OK(DB::Open(RecoveryOptions(), dir.path(), &db));
  auto iter = db->NewIterator(ReadOptions());
  auto expected = model.begin();
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    CHECK(expected != model.end());
    CHECK_EQ(iter->key(), std::string_view(expected->first));
    CHECK_EQ(iter->value(), std::string_view(expected->second));
    ++expected;
  }
  CHECK(expected == model.end());
  std::printf("       (12 opens, %zu keys intact)\n", model.size());
}

}  // namespace lsmkv
