#include "storage/sparse_index.h"

#include <algorithm>
#include <cassert>

#include "base/endian.h"

namespace storage {

void SparseIndex::add(base::u32 rel_offset, base::u32 file_pos) {
  // Appended in append order, which is already sorted. Anything else is a caller bug,
  // and a silently unsorted index turns a binary search into a wrong answer rather
  // than a slow one.
  assert(entries_.empty() ||
         (rel_offset > entries_.back().rel_offset && file_pos > entries_.back().file_pos));
  entries_.push_back(IndexEntry{rel_offset, file_pos});
}

void SparseIndex::maybe_add(base::u32 rel_offset, base::u32 file_pos, base::u32 interval_bytes) {
  if (!entries_.empty()) {
    const IndexEntry& last = entries_.back();
    // Both comparisons are written as "not yet past the last entry" rather than as a
    // subtraction: on unsigned types a caller passing a decreasing position turns a
    // subtraction into a very large number and the guard into a no-op.
    if (rel_offset <= last.rel_offset) return;
    if (file_pos <= last.file_pos) return;
    if (file_pos - last.file_pos < interval_bytes) return;  // safe: file_pos > last
  }
  add(rel_offset, file_pos);
}

IndexEntry SparseIndex::lookup(base::u32 rel_offset) const {
  const auto it = std::upper_bound(entries_.begin(), entries_.end(), rel_offset,
                                   [](base::u32 target, const IndexEntry& e) {
                                     return target < e.rel_offset;
                                   });
  if (it == entries_.begin()) return IndexEntry{};
  return *(it - 1);
}

void SparseIndex::truncate_from(base::u32 file_pos) {
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), file_pos,
                                   [](const IndexEntry& e, base::u32 target) {
                                     return e.file_pos < target;
                                   });
  entries_.erase(it, entries_.end());
}

void SparseIndex::encode(base::Buffer& out) const {
  for (const IndexEntry& e : entries_) {
    out.append_u32_le(e.rel_offset);
    out.append_u32_le(e.file_pos);
  }
}

SparseIndex SparseIndex::decode(base::Slice bytes, base::u32 log_size) {
  SparseIndex index;
  const std::size_t count = bytes.size() / kIndexEntryBytes;  // a partial tail entry is dropped
  for (std::size_t i = 0; i < count; ++i) {
    const base::u8* p = bytes.data() + i * kIndexEntryBytes;
    const IndexEntry e{base::load_u32_le(p), base::load_u32_le(p + 4)};

    if (e.file_pos >= log_size) break;
    if (!index.entries_.empty() && (e.rel_offset <= index.entries_.back().rel_offset ||
                                    e.file_pos <= index.entries_.back().file_pos)) {
      break;
    }
    index.entries_.push_back(e);
  }
  return index;
}

}  // namespace storage
