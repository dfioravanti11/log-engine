#include "storage/sparse_index.h"

#include <gtest/gtest.h>

#include <vector>

#include "base/endian.h"

namespace {

using base::Buffer;
using base::Slice;
using storage::IndexEntry;
using storage::SparseIndex;

// What lookup() has to be equivalent to. The binary search exists to make this fast,
// never to make it different.
IndexEntry scan(const SparseIndex& index, base::u32 target) {
  IndexEntry best;
  for (std::size_t i = 0; i < index.size(); ++i) {
    if (index[i].rel_offset > target) break;
    best = index[i];
  }
  return best;
}

// Irregular gaps on both axes: equal spacing would hide an off-by-one in the search.
SparseIndex make_index(base::u32 count) {
  SparseIndex index;
  for (base::u32 i = 0; i < count; ++i) {
    index.add(3u + i * 7u + (i % 3u), 128u + i * 4096u + (i % 5u));
  }
  return index;
}

Buffer encoded(const SparseIndex& index) {
  Buffer out;
  index.encode(out);
  return out;
}

Buffer raw_entries(const std::vector<IndexEntry>& entries) {
  Buffer out;
  for (const IndexEntry& e : entries) {
    out.append_u32_le(e.rel_offset);
    out.append_u32_le(e.file_pos);
  }
  return out;
}

TEST(SparseIndex, LookupAgreesWithLinearScan) {
  const SparseIndex index = make_index(300);
  const base::u32 last = index[index.size() - 1].rel_offset;

  // Probes cover before the first entry, every gap between entries, and past the last.
  for (base::u32 target = 0; target <= last + 50u; ++target) {
    const IndexEntry got = index.lookup(target);
    const IndexEntry want = scan(index, target);
    ASSERT_EQ(got.rel_offset, want.rel_offset) << "target=" << target;
    ASSERT_EQ(got.file_pos, want.file_pos) << "target=" << target;
  }
}

// A miss is not an error: scanning from the top of the segment is always correct, so
// the index can only ever save work.
TEST(SparseIndex, LookupOnEmptyIndexPointsAtTheTopOfTheSegment) {
  const SparseIndex index;
  ASSERT_TRUE(index.empty());
  EXPECT_EQ(index.lookup(0).rel_offset, 0u);
  EXPECT_EQ(index.lookup(0).file_pos, 0u);
  EXPECT_EQ(index.lookup(123456).rel_offset, 0u);
  EXPECT_EQ(index.lookup(123456).file_pos, 0u);
}

TEST(SparseIndex, MaybeAddHonorsTheByteInterval) {
  constexpr base::u32 kInterval = storage::kDefaultIndexIntervalBytes;
  SparseIndex index;

  index.maybe_add(0, 0, kInterval);
  EXPECT_EQ(index.size(), 1u);

  index.maybe_add(10, 100, kInterval);
  index.maybe_add(20, kInterval - 1u, kInterval);
  EXPECT_EQ(index.size(), 1u) << "an entry appeared before the interval elapsed";

  index.maybe_add(30, kInterval, kInterval);
  EXPECT_EQ(index.size(), 2u);

  index.maybe_add(40, 2u * kInterval - 1u, kInterval);
  EXPECT_EQ(index.size(), 2u);

  index.maybe_add(50, 2u * kInterval, kInterval);
  ASSERT_EQ(index.size(), 3u);
  EXPECT_EQ(index[2].rel_offset, 50u);
  EXPECT_EQ(index[2].file_pos, 2u * kInterval);

  // Same offset, far enough along in bytes: a duplicate rel_offset would break the
  // strict ordering the binary search assumes.
  index.maybe_add(50, 10u * kInterval, kInterval);
  EXPECT_EQ(index.size(), 3u);
}

TEST(SparseIndex, TruncateFromDropsEntriesAtOrPastPosition) {
  const SparseIndex original = make_index(4);  // file positions 128, 4225, 8322, 12419

  SparseIndex index = original;
  index.truncate_from(index[2].file_pos);
  ASSERT_EQ(index.size(), 2u);
  EXPECT_EQ(index[1].file_pos, original[1].file_pos);

  index = original;
  index.truncate_from(original[1].file_pos + 1u);
  EXPECT_EQ(index.size(), 2u);

  index = original;
  index.truncate_from(0);
  EXPECT_TRUE(index.empty());

  index = original;
  index.truncate_from(1u << 30);
  EXPECT_EQ(index.size(), original.size());
}

TEST(SparseIndex, EncodeDecodeRoundTrip) {
  const SparseIndex original = make_index(64);
  const Buffer bytes = encoded(original);
  ASSERT_EQ(bytes.size(), original.size() * storage::kIndexEntryBytes);

  const base::u32 log_size = original[original.size() - 1].file_pos + 1u;
  const SparseIndex decoded = SparseIndex::decode(bytes.slice(), log_size);

  ASSERT_EQ(decoded.size(), original.size());
  EXPECT_EQ(decoded.entries(), original.entries());
}

// The index is never fsynced (§16.1), so after a crash its tail is whatever the page
// cache happened to have flushed — including half of an 8-byte entry.
TEST(SparseIndex, DecodeDropsPartialTrailingEntry) {
  const SparseIndex original = make_index(3);
  const Buffer bytes = encoded(original);
  const base::u32 log_size = original[2].file_pos + 1u;

  for (std::size_t extra = 1; extra < storage::kIndexEntryBytes; ++extra) {
    const SparseIndex decoded =
        SparseIndex::decode(bytes.slice().subslice(0, 2 * storage::kIndexEntryBytes + extra),
                            log_size);
    EXPECT_EQ(decoded.size(), 2u) << "extra=" << extra;
    EXPECT_EQ(decoded.entries(), std::vector<IndexEntry>(original.entries().begin(),
                                                         original.entries().begin() + 2));
  }
}

// Same reason: the flushed tail can point past the end of a log whose own tail was
// truncated by recovery. Everything before the first bad entry is still usable, and
// the recovery scan rebuilds the rest.
TEST(SparseIndex, DecodeStopsAtEntryPastLogSize) {
  const Buffer bytes = raw_entries({{0, 0}, {10, 4096}, {20, 8192}});

  const SparseIndex all = SparseIndex::decode(bytes.slice(), 8193);
  EXPECT_EQ(all.size(), 3u);

  const SparseIndex clipped = SparseIndex::decode(bytes.slice(), 5000);
  ASSERT_EQ(clipped.size(), 2u);
  EXPECT_EQ(clipped[1].file_pos, 4096u);

  // A position exactly at log_size points one byte past the last record.
  const SparseIndex exact = SparseIndex::decode(bytes.slice(), 4096);
  EXPECT_EQ(exact.size(), 1u);
}

TEST(SparseIndex, DecodeStopsAtNonMonotonicPair) {
  const Buffer by_offset = raw_entries({{0, 0}, {10, 4096}, {5, 8192}, {99, 12288}});
  const SparseIndex a = SparseIndex::decode(by_offset.slice(), 1u << 20);
  ASSERT_EQ(a.size(), 2u) << "kept entries after a backwards offset";
  EXPECT_EQ(a[1].rel_offset, 10u);

  const Buffer by_position = raw_entries({{0, 0}, {10, 4096}, {20, 4096}, {30, 12288}});
  const SparseIndex b = SparseIndex::decode(by_position.slice(), 1u << 20);
  ASSERT_EQ(b.size(), 2u) << "kept entries after a stalled file position";
  EXPECT_EQ(b[1].file_pos, 4096u);
}

}  // namespace
