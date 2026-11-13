// Iterator combinators shared by the table reader and the compaction code.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lsmkv/iterator.h"
#include "table/block.h"

namespace lsmkv {

// An iterator over no entries, used where a lookup finds nothing to read.
std::unique_ptr<Iterator> NewEmptyIterator();

// An iterator that only reports an error, so a failed open can be returned
// through the same interface as a successful one.
std::unique_ptr<Iterator> NewErrorIterator(Status status);

// Flattens a two-level structure: `index` yields values that name a second
// level, and `open` turns one of those values into an iterator over it.
// Used for index block -> data block inside a table, and for the file list
// of a level -> the tables themselves.
//
// `open` may return an error iterator; its status propagates.
std::unique_ptr<Iterator> NewTwoLevelIterator(
    std::unique_ptr<Iterator> index,
    std::function<std::unique_ptr<Iterator>(std::string_view index_value)> open);

// Merges several sorted iterators into one. Where the same key appears in
// more than one child, the child earliest in `children` wins, so callers pass
// newest first.
std::unique_ptr<Iterator> NewMergingIterator(std::vector<std::unique_ptr<Iterator>> children,
                                             BlockKeyComparator cmp);

}  // namespace lsmkv
