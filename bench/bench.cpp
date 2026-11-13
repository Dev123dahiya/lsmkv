// Measures the operations the design is meant to make fast, and the ones it
// makes slow. Every number this prints came from the run that printed it.
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lsmkv/db.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Timer {
  Clock::time_point start = Clock::now();
  double Seconds() const {
    return std::chrono::duration<double>(Clock::now() - start).count();
  }
};

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%016llu", static_cast<unsigned long long>(i));
  return buf;
}

void Report(const char* name, uint64_t ops, double seconds, uint64_t bytes = 0) {
  std::printf("  %-26s %9.0f ops/s   %7.2f s", name, static_cast<double>(ops) / seconds, seconds);
  if (bytes > 0) {
    std::printf("   %6.1f MB/s",
                static_cast<double>(bytes) / seconds / 1048576.0);
  }
  std::printf("\n");
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  const uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 200000;
  const size_t value_size = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 100;
  const std::string path = argc > 3 ? argv[3] : "/tmp/lsmkv_bench";

  lsmkv::Options options;
  options.write_buffer_size = 4u << 20;
  options.max_file_size = 2u << 20;

  lsmkv::DestroyDB(path, options);
  std::unique_ptr<lsmkv::DB> db;
  const lsmkv::Status open = lsmkv::DB::Open(options, path, &db);
  if (!open.ok()) {
    std::fprintf(stderr, "open failed: %s\n", open.ToString().c_str());
    return 1;
  }

  const std::string value(value_size, 'v');
  const uint64_t payload = count * (value_size + 19);

  std::printf("lsmkv benchmark\n  %llu keys, %zu-byte values, %.1f MB of payload\n\n",
              static_cast<unsigned long long>(count), value_size,
              static_cast<double>(payload) / 1048576.0);

  // Sequential writes: keys arrive in order, so every memtable flush
  // produces a table that does not overlap the last.
  {
    Timer t;
    for (uint64_t i = 0; i < count; ++i) db->Put(lsmkv::WriteOptions(), Key(i), value);
    Report("fill sequential", count, t.Seconds(), payload);
  }

  // Random writes: the same work, but each flush spans the whole key space,
  // so compaction has far more merging to do.
  {
    lsmkv::DestroyDB(path + "_rand", options);
    std::unique_ptr<lsmkv::DB> rdb;
    lsmkv::DB::Open(options, path + "_rand", &rdb);
    std::mt19937_64 rng(1);
    Timer t;
    for (uint64_t i = 0; i < count; ++i) rdb->Put(lsmkv::WriteOptions(), Key(rng() % count), value);
    Report("fill random", count, t.Seconds(), payload);
    rdb->WaitForCompaction();
  }

  // Durable writes: each one waits for the log to reach stable storage.
  {
    const uint64_t n = std::min<uint64_t>(count / 20, 5000);
    lsmkv::WriteOptions sync;
    sync.sync = true;
    Timer t;
    for (uint64_t i = 0; i < n; ++i) db->Put(sync, Key(count + i), value);
    Report("fill sync", n, t.Seconds());
  }

  // Batched writes: one log record and one sync for many updates.
  {
    const uint64_t batches = count / 1000;
    Timer t;
    for (uint64_t b = 0; b < batches; ++b) {
      lsmkv::WriteBatch batch;
      for (uint64_t i = 0; i < 1000; ++i) batch.Put(Key(b * 1000 + i), value);
      db->Write(lsmkv::WriteOptions(), &batch);
    }
    Report("fill batched (1000/batch)", batches * 1000, t.Seconds(), payload);
  }

  db->WaitForCompaction();

  // Reads of keys that are present.
  {
    std::mt19937_64 rng(2);
    std::string got;
    uint64_t hits = 0;
    Timer t;
    for (uint64_t i = 0; i < count; ++i) {
      if (db->Get(lsmkv::ReadOptions(), Key(rng() % count), &got).ok()) ++hits;
    }
    Report("read random (hit)", count, t.Seconds());
    if (hits == 0) std::fprintf(stderr, "  warning: no keys found\n");
  }

  // Reads of keys that are absent: the case the bloom filters exist for.
  {
    std::mt19937_64 rng(3);
    std::string got;
    Timer t;
    for (uint64_t i = 0; i < count; ++i) {
      db->Get(lsmkv::ReadOptions(), Key(1'000'000'000ull + rng() % count), &got);
    }
    Report("read random (miss)", count, t.Seconds());
  }

  // Ordered scan.
  {
    Timer t;
    uint64_t seen = 0;
    auto iter = db->NewIterator(lsmkv::ReadOptions());
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) ++seen;
    Report("scan forward", seen, t.Seconds(), seen * (value_size + 19));
  }

  // Reverse scan, which has to search rather than follow a back pointer.
  {
    Timer t;
    uint64_t seen = 0;
    auto iter = db->NewIterator(lsmkv::ReadOptions());
    for (iter->SeekToLast(); iter->Valid(); iter->Prev()) ++seen;
    Report("scan backward", seen, t.Seconds());
  }

  std::printf("\n%s\n", db->Stats().c_str());
  lsmkv::DestroyDB(path, options);
  lsmkv::DestroyDB(path + "_rand", options);
  return 0;
}
