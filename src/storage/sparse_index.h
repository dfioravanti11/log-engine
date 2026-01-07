#pragma once

#include <vector>

#include "base/buffer.h"
#include "base/slice.h"
#include "base/types.h"

namespace storage {

// One entry per ~4 KB of segment, not one per batch: the index exists to bound a scan,
// not to eliminate it. A dense index would cost more memory than the page cache it is
// trying to save, and Kafka's sparse index is sparse for exactly this reason.
//
// Both fields are relative to the segment, which is what keeps them 32-bit: a segment
// is bounded well under 4 GiB, so absolute u64 offsets would waste half the structure.
struct IndexEntry {
  base::u32 rel_offset = 0;  // offset - segment base offset
  base::u32 file_pos = 0;    // byte position of that batch in the .log file

  friend bool operator==(const IndexEntry& a, const IndexEntry& b) {
    return a.rel_offset == b.rel_offset && a.file_pos == b.file_pos;
  }
};

inline constexpr std::size_t kIndexEntryBytes = 8;
inline constexpr base::u32 kDefaultIndexIntervalBytes = 4096;

// Sorted vector, binary-searched. **Never fsynced** (§16.1) — it is a rebuildable hint,
// which is precisely why decode() distrusts every byte it reads back.
class SparseIndex {
 public:
  void clear() { entries_.clear(); }

  [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const IndexEntry& operator[](std::size_t i) const { return entries_[i]; }
  [[nodiscard]] const std::vector<IndexEntry>& entries() const noexcept { return entries_; }

  void add(base::u32 rel_offset, base::u32 file_pos);

  // Adds an entry only if `interval_bytes` have passed since the last one. The caller
  // is the append path, which knows where every batch starts; this decides which of
  // those starts is worth remembering.
  void maybe_add(base::u32 rel_offset, base::u32 file_pos, base::u32 interval_bytes);

  // Greatest entry with rel_offset <= target. Returns {0, 0} when the index is empty
  // or the target precedes the first entry — never a miss, because "scan from the top
  // of the segment" is always a correct answer. The index can only ever save work.
  [[nodiscard]] IndexEntry lookup(base::u32 rel_offset) const;

  // Drops every entry pointing at or past `file_pos`. Used when recovery truncates a
  // torn tail: an index entry pointing into bytes that no longer exist is worse than
  // no entry at all.
  void truncate_from(base::u32 file_pos);

  void encode(base::Buffer& out) const;

  // Reads an index file back, stopping at the first entry that fails a sanity check:
  // a partial trailing entry, a non-increasing pair, or a position at/past `log_size`.
  //
  // All three are *expected* after a crash. The index is never fsynced, so its tail is
  // whatever the page cache happened to have flushed — possibly nothing, possibly half
  // an entry. Everything up to the first bad entry is still usable, and everything
  // after it gets rebuilt by the recovery scan.
  static SparseIndex decode(base::Slice bytes, base::u32 log_size);

 private:
  std::vector<IndexEntry> entries_;
};

}  // namespace storage
