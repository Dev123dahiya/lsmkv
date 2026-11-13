#include "table/sstable.h"

#include <cassert>
#include <cstdio>

#include "db/dbformat.h"
#include "table/iterators.h"
#include "table/table_cache.h"
#include "util/bloom.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsmkv {
namespace {

// Name under which the table's bloom filter is recorded in the metaindex.
constexpr std::string_view kFilterBlockName = "filter.bloom.v1";

}  // namespace

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : options_(options),
      file_(file),
      data_block_(options.block_restart_interval),
      // Index entries are one per block and always full keys, so a restart
      // every entry costs little and makes index seeks branch-free.
      index_block_(1) {
  if (options_.bloom_bits_per_key > 0) {
    filter_ = std::make_unique<BloomFilterBuilder>(options_.bloom_bits_per_key);
  }
}

TableBuilder::~TableBuilder() = default;

void TableBuilder::Add(std::string_view key, std::string_view value) {
  if (!status_.ok()) return;
  assert(!closed_);
  assert(num_entries_ == 0 || CompareInternalKey(key, last_key_) > 0);

  if (pending_index_entry_) {
    // The previous block has been written; now that the first key of the new
    // block is known, a short separator between them can be stored.
    assert(data_block_.empty());
    const std::string separator = ShortestInternalSeparator(last_key_, key);
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(separator, handle_encoding);
    pending_index_entry_ = false;
  }

  if (filter_) filter_->Add(ExtractUserKey(key));

  last_key_.assign(key);
  ++num_entries_;
  data_block_.Add(key, value);

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) FlushDataBlock();
}

void TableBuilder::FlushDataBlock() {
  if (data_block_.empty()) return;
  assert(!pending_index_entry_);
  WriteRawBlock(data_block_.Finish(), &pending_handle_);
  data_block_.Reset();
  if (status_.ok()) {
    pending_index_entry_ = true;
    status_ = file_->Flush();
  }
}

void TableBuilder::WriteRawBlock(std::string_view contents, BlockHandle* handle) {
  handle->set_offset(offset_);
  handle->set_size(contents.size());
  status_ = file_->Append(contents);
  if (!status_.ok()) return;

  // Compression is not implemented; the byte is written so that a reader
  // from a future build can tell what it is looking at.
  char trailer[kBlockTrailerSize];
  trailer[0] = static_cast<char>(CompressionType::kNone);
  uint32_t crc = crc32c::Value(contents);
  crc = crc32c::Extend(crc, trailer, 1);
  const uint32_t masked = crc32c::Mask(crc);
  std::memcpy(trailer + 1, &masked, sizeof(masked));

  status_ = file_->Append(std::string_view(trailer, kBlockTrailerSize));
  if (status_.ok()) offset_ += contents.size() + kBlockTrailerSize;
}

Status TableBuilder::Finish() {
  FlushDataBlock();
  assert(!closed_);
  closed_ = true;
  if (!status_.ok()) return status_;

  // One bloom filter covers the whole table. Tables are bounded by
  // max_file_size, so the filter stays small enough to hold in memory
  // alongside the index, and a point lookup checks it once rather than
  // locating a per-region filter first.
  BlockHandle filter_handle;
  bool has_filter = false;
  if (filter_ && filter_->KeyCount() > 0) {
    std::string filter_contents;
    filter_->Finish(&filter_contents);
    WriteRawBlock(filter_contents, &filter_handle);
    has_filter = status_.ok();
    if (!status_.ok()) return status_;
  }

  // The metaindex names the optional blocks, so a reader that does not know
  // a name can skip it instead of failing.
  BlockHandle metaindex_handle;
  {
    BlockBuilder meta_block(options_.block_restart_interval);
    if (has_filter) {
      std::string handle_encoding;
      filter_handle.EncodeTo(&handle_encoding);
      meta_block.Add(kFilterBlockName, handle_encoding);
    }
    WriteRawBlock(meta_block.Finish(), &metaindex_handle);
    if (!status_.ok()) return status_;
  }

  BlockHandle index_handle;
  {
    if (pending_index_entry_) {
      const std::string successor = ShortInternalSuccessor(last_key_);
      std::string handle_encoding;
      pending_handle_.EncodeTo(&handle_encoding);
      index_block_.Add(successor, handle_encoding);
      pending_index_entry_ = false;
    }
    WriteRawBlock(index_block_.Finish(), &index_handle);
    if (!status_.ok()) return status_;
  }

  Footer footer;
  footer.set_metaindex_handle(metaindex_handle);
  footer.set_index_handle(index_handle);
  std::string footer_encoding;
  footer.EncodeTo(&footer_encoding);
  status_ = file_->Append(footer_encoding);
  if (status_.ok()) offset_ += footer_encoding.size();
  return status_;
}

void TableBuilder::Abandon() {
  assert(!closed_);
  closed_ = true;
}

Status ReadBlockContents(RandomAccessFile* file, const ReadOptions& options,
                         const BlockHandle& handle, std::string* result) {
  const size_t n = static_cast<size_t>(handle.size());
  // A corrupt handle could ask for an enormous allocation, so bound it
  // before reserving anything.
  if (handle.size() > (1ull << 31)) return Status::Corruption("block handle size is implausible");

  std::string buf(n + kBlockTrailerSize, '\0');
  std::string_view contents;
  LSMKV_TRY(file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf.data()));
  if (contents.size() != n + kBlockTrailerSize) {
    return Status::Corruption("truncated block read");
  }

  const char* data = contents.data();
  if (options.verify_checksums) {
    const uint32_t expected = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != expected) return Status::Corruption("block checksum mismatch");
  }

  const auto compression = static_cast<CompressionType>(static_cast<uint8_t>(data[n]));
  if (compression != CompressionType::kNone) {
    return Status::NotSupported("block is compressed; this build has no decompressor");
  }

  result->assign(data, n);
  return Status::OK();
}

struct Table::Rep {
  Options options;
  RandomAccessFile* file = nullptr;
  uint64_t cache_id = 0;
  BlockCache* cache = nullptr;
  std::unique_ptr<Block> index_block;
  std::string filter_contents;  // empty when the table has no filter
  BlockHandle metaindex_handle;
};

Table::Table(std::unique_ptr<Rep> rep) : rep_(std::move(rep)) {}
Table::~Table() = default;

Status Table::Open(const Options& options, RandomAccessFile* file, uint64_t file_size,
                   uint64_t cache_id, BlockCache* cache, std::unique_ptr<Table>* result) {
  if (file_size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be a table");
  }

  char footer_space[Footer::kEncodedLength];
  std::string_view footer_input;
  LSMKV_TRY(file->Read(file_size - Footer::kEncodedLength, Footer::kEncodedLength, &footer_input,
                       footer_space));
  Footer footer;
  LSMKV_TRY(footer.DecodeFrom(footer_input));

  ReadOptions read_options;
  read_options.verify_checksums = options.paranoid_checks;

  std::string index_contents;
  LSMKV_TRY(ReadBlockContents(file, read_options, footer.index_handle(), &index_contents));

  auto rep = std::make_unique<Rep>();
  rep->options = options;
  rep->file = file;
  rep->cache_id = cache_id;
  rep->cache = cache;
  rep->metaindex_handle = footer.metaindex_handle();
  LSMKV_TRY(Block::Create(std::move(index_contents), CompareInternal, &rep->index_block));

  // The filter is optional. A table written without one, or with a filter
  // this build does not recognise, still reads correctly — just slower.
  std::string metaindex_contents;
  const Status meta_status =
      ReadBlockContents(file, read_options, footer.metaindex_handle(), &metaindex_contents);
  if (meta_status.ok()) {
    std::unique_ptr<Block> meta_block;
    if (Block::Create(std::move(metaindex_contents), CompareBytewise, &meta_block).ok()) {
      auto iter = meta_block->NewIterator();
      iter->Seek(kFilterBlockName);
      if (iter->Valid() && iter->key() == kFilterBlockName) {
        BlockHandle filter_handle;
        std::string_view handle_value = iter->value();
        if (filter_handle.DecodeFrom(&handle_value).ok()) {
          // A filter that fails to load is not fatal.
          ReadBlockContents(file, read_options, filter_handle, &rep->filter_contents);
        }
      }
    }
  }

  result->reset(new Table(std::move(rep)));
  return Status::OK();
}

std::unique_ptr<Iterator> Table::BlockReader(const ReadOptions& options,
                                             std::string_view index_value) const {
  BlockHandle handle;
  std::string_view input = index_value;
  const Status s = handle.DecodeFrom(&input);
  if (!s.ok()) return NewErrorIterator(s);

  std::shared_ptr<Block> block;
  if (rep_->cache != nullptr) {
    block = rep_->cache->Lookup(rep_->cache_id, handle.offset());
  }
  if (block == nullptr) {
    std::string contents;
    const Status read_status = ReadBlockContents(rep_->file, options, handle, &contents);
    if (!read_status.ok()) return NewErrorIterator(read_status);

    std::unique_ptr<Block> owned;
    const Status create_status = Block::Create(std::move(contents), CompareInternal, &owned);
    if (!create_status.ok()) return NewErrorIterator(create_status);
    block = std::shared_ptr<Block>(std::move(owned));

    if (rep_->cache != nullptr && options.fill_cache) {
      rep_->cache->Insert(rep_->cache_id, handle.offset(), block);
    }
  }

  // The iterator points into the block, so it has to keep the block alive
  // even if the cache evicts it in the meantime.
  class BlockOwningIterator final : public Iterator {
   public:
    BlockOwningIterator(std::shared_ptr<Block> block, std::unique_ptr<Iterator> iter)
        : block_(std::move(block)), iter_(std::move(iter)) {}

    bool Valid() const override { return iter_->Valid(); }
    void SeekToFirst() override { iter_->SeekToFirst(); }
    void SeekToLast() override { iter_->SeekToLast(); }
    void Seek(std::string_view target) override { iter_->Seek(target); }
    void Next() override { iter_->Next(); }
    void Prev() override { iter_->Prev(); }
    std::string_view key() const override { return iter_->key(); }
    std::string_view value() const override { return iter_->value(); }
    Status status() const override { return iter_->status(); }

   private:
    std::shared_ptr<Block> block_;
    std::unique_ptr<Iterator> iter_;
  };

  auto inner = block->NewIterator();
  return std::make_unique<BlockOwningIterator>(std::move(block), std::move(inner));
}

std::unique_ptr<Iterator> Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(rep_->index_block->NewIterator(),
                             [this, options](std::string_view index_value) {
                               return BlockReader(options, index_value);
                             });
}

Status Table::InternalGet(
    const ReadOptions& options, std::string_view key,
    const std::function<void(std::string_view, std::string_view)>& handle_result) const {
  // The filter covers the whole table, so it is checked first: a miss costs
  // one hash and no I/O at all, not even an index walk.
  if (!rep_->filter_contents.empty() &&
      !BloomMayContain(rep_->filter_contents, ExtractUserKey(key))) {
    filter_skips_.fetch_add(1, std::memory_order_relaxed);
    return Status::OK();
  }

  auto index_iter = rep_->index_block->NewIterator();
  index_iter->Seek(key);
  if (!index_iter->Valid()) return index_iter->status();

  auto block_iter = BlockReader(options, index_iter->value());
  block_iter->Seek(key);
  if (block_iter->Valid()) handle_result(block_iter->key(), block_iter->value());
  Status s = block_iter->status();
  if (s.ok()) s = index_iter->status();
  return s;
}

uint64_t Table::ApproximateOffsetOf(std::string_view key) const {
  auto index_iter = rep_->index_block->NewIterator();
  index_iter->Seek(key);
  if (!index_iter->Valid()) {
    // Past the last block: the metaindex marks the end of the data.
    return rep_->metaindex_handle.offset();
  }
  BlockHandle handle;
  std::string_view input = index_iter->value();
  if (!handle.DecodeFrom(&input).ok()) return rep_->metaindex_handle.offset();
  return handle.offset();
}

}  // namespace lsmkv
