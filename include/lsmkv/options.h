#pragma once

#include <cstddef>

namespace lsmkv {

class Snapshot;
class Env;

struct Options {
  // Create the database if the directory holds no CURRENT file.
  bool create_if_missing = true;

  // Fail Open if the database already exists.
  bool error_if_exists = false;

  // Verify block checksums on every read, not only during recovery. Costs a
  // CRC over each block read; catches bit rot before it reaches the caller.
  bool paranoid_checks = true;

  // How much a memtable may hold before it is frozen and flushed. Larger
  // values mean fewer, bigger tables and less write amplification, at the
  // cost of memory and longer recovery.
  size_t write_buffer_size = 4u << 20;

  // Target size of a table file produced by a flush or compaction.
  size_t max_file_size = 2u << 20;

  // Uncompressed size of a data block. Reads fetch a whole block, so this
  // trades read amplification against index size.
  size_t block_size = 4096;

  // How often a data block stores a full key instead of a shared prefix.
  // Smaller means faster seeks within a block and a larger block.
  int block_restart_interval = 16;

  // Bits of bloom filter per key; 0 disables filters. 10 bits costs about
  // 1.25 bytes per key for roughly a 1% false positive rate.
  int bloom_bits_per_key = 10;

  // Table readers held open. Each costs a file descriptor plus its index and
  // filter blocks.
  size_t max_open_files = 128;

  // Decompressed data blocks cached in memory across reads.
  size_t block_cache_bytes = 8u << 20;

  // Number of level-0 files at which writes are slowed, and the number at
  // which they are stopped until compaction catches up. Level-0 files
  // overlap, so every one of them has to be consulted on a read.
  int level0_slowdown_trigger = 8;
  int level0_stop_trigger = 12;

  // Level-0 file count that starts a compaction into level 1.
  int level0_compaction_trigger = 4;

  // The filesystem the database runs on. Null uses the real one. The crash
  // tests install an Env that stops writing at a chosen byte, which is how
  // the durability guarantee is checked rather than assumed.
  Env* env = nullptr;
};

struct ReadOptions {
  // Check the CRC of each block this read touches.
  bool verify_checksums = true;

  // Populate the block cache with blocks this read pulls in. Turn off for a
  // full scan so it does not evict the working set.
  bool fill_cache = true;

  // Read the database as of this snapshot. Null reads the latest state.
  const Snapshot* snapshot = nullptr;
};

struct WriteOptions {
  // Flush the write-ahead log to stable storage before returning. Without
  // it a write survives a process crash but not a machine crash.
  bool sync = false;
};

}  // namespace lsmkv
