# lsmkv

An embedded ordered key-value store in C++20, built as a log-structured merge tree — the structure behind LevelDB, RocksDB and Cassandra's storage layer. Written from scratch to make the mechanics readable: the write-ahead log, the skip list, the sorted table format, the bloom filters and the compaction policy are each about as small as they can be while still being correct.

No dependencies. CMake, a C++20 compiler, and nothing else.

```
75 tests, 331,926 assertions, clean under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer.
```

## What it does

```cpp
#include "lsmkv/db.h"

std::unique_ptr<lsmkv::DB> db;
lsmkv::DB::Open(lsmkv::Options{}, "/tmp/mydb", &db);

db->Put(lsmkv::WriteOptions{}, "key", "value");

std::string value;
lsmkv::Status s = db->Get(lsmkv::ReadOptions{}, "key", &value);

// Ordered iteration, over a consistent snapshot.
auto iter = db->NewIterator(lsmkv::ReadOptions{});
for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
  use(iter->key(), iter->value());
}
```

Keys and values are arbitrary bytes. Iteration is in byte order, forwards and backwards. A `WriteBatch` applies atomically. A `Snapshot` fixes a point in the write history that later writes cannot disturb.

## How it works

```
       Put / Delete
            │
            ▼
   ┌─────────────────┐        ┌──────────────────────────┐
   │ write-ahead log │────────│ durability: a write is   │
   │ (append, CRC32C)│        │ acknowledged once this   │
   └────────┬────────┘        │ record is on disk        │
            │                 └──────────────────────────┘
            ▼
   ┌─────────────────┐
   │  memtable       │  skip list, arena-allocated
   │  (skip list)    │  one writer, lock-free readers
   └────────┬────────┘
            │  full → frozen, flushed by the background thread
            ▼
   level 0  ┌────┐┌────┐┌────┐      overlapping; every file
            └────┘└────┘└────┘      is consulted on a read
            │  compaction merges them downwards
            ▼
   level 1  ┌──────────────────┐    sorted, non-overlapping,
            └──────────────────┘    one file per key range
            ▼
   level 2  ┌──────────────────────────────┐   10x larger
            └──────────────────────────────┘
```

A write goes to the log and an in-memory table. When that table fills it is frozen and written out as a sorted table file, and background compaction merges those files down through the levels. A read consults the memtables and then each level in turn, stopping at the newest version of the key.

Nothing is ever updated in place. An overwrite or a delete appends a new record stamped with a higher sequence number, which is what makes snapshots and crash recovery possible at all: a reader at sequence N sees exactly the writes at or below N, and recovery is a replay rather than a repair.

## Measured

Numbers below are from one run of `lsmkv_bench` on a shared cloud container — 2 vCPU Intel Xeon at 2.10 GHz, 7 GB RAM, Ubuntu 24.04, GCC 13.3, `-O2`. 200,000 keys, 100-byte values. **Throughput will differ on your machine.** The shape of the results will not.

| Operation | Throughput | Notes |
| --- | --- | --- |
| fill sequential | 824,944 ops/s | 93.6 MB/s |
| fill random | 727,301 ops/s | 82.5 MB/s — the same work, more merging |
| fill batched, 1000 per batch | 873,748 ops/s | one log record per batch |
| fill with `sync` | 9,111 ops/s | one `fdatasync` per write |
| read random, key present | 373,983 ops/s | |
| read random, key absent | 4,069,836 ops/s | **11x faster than a hit** |
| scan forward | 5,987,992 ops/s | 679.6 MB/s |
| scan backward | 4,941,452 ops/s | no back pointers; each step searches |

Two of those rows are the whole design argument.

**A miss is eleven times faster than a hit.** That is the bloom filters. Without them a lookup for an absent key costs one block read per table it might be in; with them it usually costs one hash and no I/O at all. Measured directly in the table tests: of 2,000 lookups for keys that were never written, 1,990 were answered by the filter alone, at 10 bits per key — 1.25 bytes of memory per key to avoid a disk read.

**A durable write is ninety times slower than a buffered one.** 9,111 against 824,944 ops/s is not a flaw in the log; it is what waiting for a platter or an SSD's write cache actually costs. It is in the table because the honest version of "fast writes" has this row next to it. Batching is the way out: one `fdatasync` for a thousand updates rather than a thousand of them.

Compaction over that run: 405,000 writes produced 197,000 dropped keys and left 12 files across two levels. Most of what an LSM tree writes is garbage it later reclaims, which is the price it pays for turning random writes into sequential ones.

## Design decisions worth defending

**Immutable versions behind a `shared_ptr`.** The file layout is a `Version` object that is never mutated. Compaction builds a new one and swaps it in. A reader takes `shared_ptr<const Version>` under a lock held for a few instructions and then does all its work without any lock at all; a compaction that finishes mid-read cannot delete a file out from under it, because the file is unlinked only when the last `Version` naming it is destroyed. This replaces LevelDB's hand-rolled reference counting with ownership the compiler enforces.

**One writer, lock-free readers in the skip list.** Nodes are arena-allocated and never freed or moved while the memtable lives, so a reader holding a `Node*` can always dereference it. A node becomes visible through a release store to a predecessor's link, matched by an acquire load in `Next()`. Verified under ThreadSanitizer across 57,000 concurrent scans, not merely argued for in a comment.

**The log format resynchronises at block boundaries.** The file is a sequence of 32 KiB blocks, and a record too large for the space left is split into fragments, each with its own header and CRC32C. A reader that meets damage can skip to the next block instead of scanning bytes hoping to find a header. The test truncates a log at 584 different points and checks that every completely-written record is recovered and that the torn tail is reported as a crash rather than corruption.

**The index stores shortened separators, not whole keys.** Between two blocks the index only needs some key that sorts after the first and before the second, so `"key000000000104"` can separate two blocks whose real boundary keys are far longer. Shortening an *internal* key is a trap, though — it cuts into the 8-byte sequence tag and produces something that is no longer a key at all. Doing exactly that was a real bug here, caught by a seek test, and fixed by shortening the user key and re-stamping it with the largest possible tag.

**Compaction is where the cost is paid.** Level 0 holds freshly flushed memtables whose key ranges overlap, so every one of them is consulted on a read. Below that each level is sorted, disjoint and ten times larger than the last, which bounds a read to one file per level and total write amplification to about 10 per level. When level 0 grows faster than compaction can drain it, writes are stalled on purpose: that trades a pause now for bounded read latency, instead of letting reads degrade without limit.

**Tombstones outlive the data they hide.** A delete writes a marker, not a hole. That marker can only be dropped once no deeper level could still hold an older value for the key, and no live snapshot could still need it — otherwise deleting a key would make an older version of it reappear. There is a test for exactly that resurrection.

## What the tests check

`75 tests, 331,926 assertions`, run against the same binary under three sanitizers.

The ones that matter most:

- **Differential testing against `std::map`.** 20,000 random operations — puts, deletes, atomic batches, point reads and 380 close-and-reopen cycles — with every read, every full scan and every reopen checked against an ordered map. This is the test that catches what unit tests miss.
- **Crash recovery.** A filesystem wrapper that silently drops every write after a chosen byte, simulating a machine that stops mid-operation. Eight crash points across a run; after each, the database must reopen, every value it returns must be the one written for that key, and a full scan must still be ordered. Separately, the log is truncated at a range of offsets with the same requirement.
- **CRC-32C against the published vectors.** The checksums are pinned to RFC 3720's check values, and the SSE4.2 hardware path is compared against the portable table at every length and alignment — so the test cannot pass by both paths being wrong in the same way.
- **Bloom false-positive rate against theory.** Measured at 4, 10 and 16 bits per key and compared to `(1 - e^(-kn/m))^k`, rather than asserting the filter merely exists.
- **Concurrency.** One writer and three readers against the live database; readers verify ordering and completeness throughout, under ThreadSanitizer.
- **Corruption.** A flipped bit in a table file must surface as an error, never as a wrong answer.

Two bugs the sanitizers caught that the tests had not: a data race on the version counters between the write path and the background compaction thread, and two dangling references in the tests themselves. Both are fixed; the first is why `VersionSet` uses atomics and a dedicated manifest mutex.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lsmkv_tests          # the whole suite
./build/lsmkv_bench          # the numbers above
```

With the sanitizers:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DLSMKV_ASAN=ON && cmake --build build-asan -j
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DLSMKV_TSAN=ON && cmake --build build-tsan -j
```

There is a command line tool for poking at a database without writing a program:

```sh
./build/lsmkv_cli /tmp/mydb put greeting hello
./build/lsmkv_cli /tmp/mydb get greeting
./build/lsmkv_cli /tmp/mydb scan
./build/lsmkv_cli /tmp/mydb stats
```

## Layout

```
include/lsmkv/      public API: db.h, options.h, status.h, iterator.h, write_batch.h
src/util/           CRC32C (SSE4.2 + portable), varint coding, arena, bloom filter, Env
src/memtable/       concurrent skip list, memtable
src/wal/            the record log, shared by the write-ahead log and the manifest
src/table/          block format, sorted table reader and writer, caches, iterators
src/db/             internal key format, versions and manifest, compaction, the database
tests/              75 tests and the fault-injecting filesystem
bench/  tools/      benchmark driver, command line tool
```

About 6,000 lines of library code and 2,700 lines of tests.

## Limits

This is a readable implementation of the structure, not a replacement for RocksDB. Being specific about what it does not do:

- **No compression.** Blocks are written uncompressed. The format carries a compression-type byte so a file written by a future build is rejected with a clear message rather than misread, but no codec is implemented. Real deployments get a large win here.
- **One writer at a time.** Writes are serialised on a mutex. There is no group commit, so concurrent writers do not share a `fdatasync` the way RocksDB's write batching does. The `fill batched` row is the way to get that benefit today.
- **No column families, no transactions, no merge operators, no TTL.** `WriteBatch` is atomic, but there is no multi-statement transaction with conflict detection.
- **Compaction is single-threaded.** One background thread does all flushing and merging. A write-heavy workload on many cores would want several.
- **The block cache is one shared LRU behind one mutex.** At high concurrency that mutex would become the bottleneck; production caches sit behind many shards.
- **Level sizing is fixed**, not adaptive: level 1 is 10 MB and each level below it is ten times larger. There is no dynamic level sizing and no universal/tiered compaction option.
- **No `Env` for Windows.** The filesystem layer is POSIX. The interface is there — the crash tests use a second implementation of it — but only one is shipped.
- **Recovery reads every log file newer than the manifest's checkpoint.** A database killed with a very large unflushed memtable takes proportionally long to reopen.
- **Benchmarks are from a 2 vCPU shared container.** They are an order of magnitude, not a specification, and every one was produced by the committed benchmark driver rather than estimated.

## Licence

MIT.
