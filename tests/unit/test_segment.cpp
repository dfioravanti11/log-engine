#include "storage/segment.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "io/real/real_disk.h"
#include "storage/record_batch.h"
#include "support/temp_dir.h"

namespace {

using base::MutSlice;
using base::Slice;
using storage::BatchBuilder;
using storage::BatchMeta;
using storage::Segment;
using testsupport::record_payload;

// Fails one pread, at a chosen call index, and otherwise gets out of the way. The
// simulator found the bug this exists to pin down (seed 1, `--io-errors 0.02`); a unit
// test is what keeps it found.
class FlakyDisk final : public io::Disk {
 public:
  FlakyDisk(io::Disk& inner, int fail_pread_at) : inner_(inner), fail_at_(fail_pread_at) {}

  base::Result<io::FileId> open(std::string_view path, io::OpenMode mode) override {
    return inner_.open(path, mode);
  }
  void close(io::FileId file) override { inner_.close(file); }
  base::Result<std::size_t> pwrite(io::FileId file, Slice data, base::u64 offset) override {
    return inner_.pwrite(file, data, offset);
  }
  base::Result<std::size_t> pread(io::FileId file, MutSlice out, base::u64 offset) override {
    if (preads_++ == fail_at_) return base::fail(base::ErrorCode::kIoError);
    return inner_.pread(file, out, offset);
  }
  base::Status fsync(io::FileId file) override { return inner_.fsync(file); }
  base::Status truncate(io::FileId file, base::u64 size) override {
    ++truncates_;
    return inner_.truncate(file, size);
  }
  base::Result<base::u64> size(io::FileId file) override { return inner_.size(file); }
  base::Status remove(std::string_view path) override { return inner_.remove(path); }
  base::Status make_directories(std::string_view path) override {
    return inner_.make_directories(path);
  }
  base::Result<std::vector<std::string>> list_directory(std::string_view path) override {
    return inner_.list_directory(path);
  }

  [[nodiscard]] int preads() const { return preads_; }
  [[nodiscard]] int truncates() const { return truncates_; }

  // Arms the failure for the very next pread, whenever that turns out to be. Lets a
  // test open a segment cleanly and then break only the read path.
  void fail_next_pread() { fail_at_ = preads_; }

 private:
  io::Disk& inner_;
  int fail_at_;
  int preads_ = 0;
  int truncates_ = 0;
};

class SegmentTest : public testsupport::TempDirTest {
 protected:
  Segment::Options options() const {
    Segment::Options o;
    o.max_bytes = 1u << 20;
    o.index_interval_bytes = index_interval_;
    return o;
  }

  std::unique_ptr<Segment> open(base::u64 base_offset = 0) {
    auto seg = Segment::open(disk_, dir(), base_offset, options());
    EXPECT_TRUE(seg.ok()) << base::to_string(seg.error());
    return seg.ok() ? std::move(seg).value() : nullptr;
  }

  // Appends one batch of `records` records and returns the base offset it landed at.
  base::u64 append_batch(Segment& seg, base::u32 records, base::u64 first_record_id) {
    builder_.clear();
    for (base::u32 i = 0; i < records; ++i) {
      const std::string payload = record_payload(first_record_id + i);
      EXPECT_TRUE(builder_.add_record(Slice::from_string(payload)).ok());
    }
    const base::u64 base_offset = seg.next_offset();
    const Slice framed = builder_.finish(base_offset, BatchMeta{});
    auto header = storage::decode_header(framed);
    EXPECT_TRUE(header.ok());
    EXPECT_TRUE(seg.append(framed, header.value()).ok());
    return base_offset;
  }

  std::string log_path() const { return path(Segment::log_name(0)); }
  std::string index_path() const { return path(Segment::index_name(0)); }

  base::u64 file_size(const std::string& p) const {
    return static_cast<base::u64>(std::filesystem::file_size(p));
  }

  void poke(const std::string& p, base::u64 at, char value) {
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekp(static_cast<std::streamoff>(at));
    f.put(value);
  }

  io::real::RealDisk disk_;
  BatchBuilder builder_;
  base::u32 index_interval_ = storage::kDefaultIndexIntervalBytes;
};

TEST_F(SegmentTest, NameRoundTrip) {
  EXPECT_EQ(Segment::log_name(0), "00000000000000000000.log");
  EXPECT_EQ(Segment::log_name(524288), "00000000000000524288.log");
  EXPECT_EQ(Segment::index_name(7), "00000000000000000007.index");

  auto parsed = Segment::parse_log_name("00000000000000524288.log");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(parsed.value(), 524288u);

  // Everything that is not one of ours is rejected rather than guessed at.
  EXPECT_FALSE(Segment::parse_log_name("00000000000000524288.index").ok());
  EXPECT_FALSE(Segment::parse_log_name("raft.state").ok());
  EXPECT_FALSE(Segment::parse_log_name("0000000000000052428x.log").ok());
  EXPECT_FALSE(Segment::parse_log_name("524288.log").ok());
}

TEST_F(SegmentTest, AppendAndReadBack) {
  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_TRUE(seg->empty());

  EXPECT_EQ(append_batch(*seg, 3, 0), 0u);
  EXPECT_EQ(append_batch(*seg, 2, 3), 3u);
  EXPECT_EQ(seg->next_offset(), 5u);

  std::vector<base::u8> buf(4096);
  auto got = seg->read(0, MutSlice(buf.data(), buf.size()));
  ASSERT_TRUE(got.ok());
  ASSERT_GT(got.value(), 0u);

  // Walk the returned bytes: two whole batches, five records, in order.
  std::size_t pos = 0;
  base::u64 next_record = 0;
  int batches = 0;
  while (pos < got.value()) {
    const Slice framed = Slice(buf.data(), got.value()).subslice(pos);
    ASSERT_TRUE(storage::validate_batch(framed).ok());
    auto header = storage::decode_header(framed);
    ASSERT_TRUE(header.ok());

    storage::RecordIterator it(framed);
    Slice value;
    while (it.next(&value)) {
      EXPECT_EQ(value, Slice::from_string(record_payload(next_record)));
      ++next_record;
    }
    pos += header.value().total_bytes();
    ++batches;
  }
  EXPECT_EQ(batches, 2);
  EXPECT_EQ(next_record, 5u);
}

TEST_F(SegmentTest, ReadStartsAtTheBatchContainingTheOffset) {
  auto seg = open();
  ASSERT_NE(seg, nullptr);
  append_batch(*seg, 4, 0);   // offsets 0..3
  append_batch(*seg, 4, 4);   // offsets 4..7

  std::vector<base::u8> buf(4096);
  for (base::u64 offset = 0; offset < 8; ++offset) {
    auto got = seg->read(offset, MutSlice(buf.data(), buf.size()));
    ASSERT_TRUE(got.ok()) << "offset " << offset;
    auto header = storage::decode_header(Slice(buf.data(), got.value()));
    ASSERT_TRUE(header.ok());
    // The batch *containing* the offset, not the one starting at it — a consumer
    // asking for offset 2 gets the batch holding 0..3 and skips what it has seen.
    EXPECT_EQ(header.value().base_offset, offset < 4 ? 0u : 4u) << "offset " << offset;
  }
}

TEST_F(SegmentTest, ReadPastTheEndReturnsNothing) {
  auto seg = open();
  ASSERT_NE(seg, nullptr);
  append_batch(*seg, 1, 0);

  std::vector<base::u8> buf(4096);
  auto got = seg->read(seg->next_offset(), MutSlice(buf.data(), buf.size()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), 0u);
}

// A caught-up reader and a reader whose buffer is too small must not look the same.
TEST_F(SegmentTest, ReadFailsRatherThanReturnEmptyWhenTheBufferIsTooSmall) {
  auto seg = open();
  ASSERT_NE(seg, nullptr);
  append_batch(*seg, 4, 0);

  std::vector<base::u8> buf(storage::kBatchHeaderBytes);
  auto got = seg->read(0, MutSlice(buf.data(), buf.size()));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error(), base::ErrorCode::kMessageTooLarge);
}

TEST_F(SegmentTest, ReadReturnsWholeBatchesOnly) {
  auto seg = open();
  ASSERT_NE(seg, nullptr);
  append_batch(*seg, 2, 0);
  const base::u64 first_batch_bytes = file_size(log_path());
  append_batch(*seg, 2, 2);

  // A buffer one byte short of two batches must come back holding exactly one.
  std::vector<base::u8> buf(file_size(log_path()) - 1);
  auto got = seg->read(0, MutSlice(buf.data(), buf.size()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), first_batch_bytes);
}

// Corruption that appears *after* recovery has already blessed the segment. Recovery
// validates once, at open; a bit that flips later is only ever seen by the read path,
// so the read path has to be the one that catches it (§17 — CRC on read, refuse to
// serve). Returning wrong bytes with an ok status is the one thing this layer must
// never do, and it is exactly what a header-only check would have done here.
TEST_F(SegmentTest, ReadRefusesToServeABatchThatWentBadAfterRecovery) {
  base::u64 first_batch_bytes = 0;
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    append_batch(*seg, 2, 0);
    first_batch_bytes = file_size(log_path());
    append_batch(*seg, 2, 2);
    ASSERT_TRUE(seg->fsync().ok());
    ASSERT_TRUE(seg->flush_index().ok());
  }

  auto seg = open();
  ASSERT_NE(seg, nullptr);
  ASSERT_EQ(seg->next_offset(), 4u);

  std::vector<base::u8> buf(4096);
  ASSERT_TRUE(seg->read(0, MutSlice(buf.data(), buf.size())).ok());

  // Rot one byte of the second batch's payload behind the segment's back.
  poke(log_path(), first_batch_bytes + storage::kBatchHeaderBytes + 2, '!');

  // The first batch is still fine and still served.
  auto first = seg->read(0, MutSlice(buf.data(), first_batch_bytes));
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value(), first_batch_bytes);

  // The damaged one is refused rather than handed back.
  auto damaged = seg->read(2, MutSlice(buf.data(), buf.size()));
  ASSERT_FALSE(damaged.ok());
  EXPECT_EQ(damaged.error(), base::ErrorCode::kCorruptRecord);

  // And a read spanning both must not quietly return the good prefix as if it were
  // everything — that would look identical to "the log ends here" to a consumer.
  auto both = seg->read(0, MutSlice(buf.data(), buf.size()));
  ASSERT_FALSE(both.ok());
  EXPECT_EQ(both.error(), base::ErrorCode::kCorruptRecord);
}

TEST_F(SegmentTest, AppendRejectsAnOffsetThatDoesNotContinueTheLog) {
  auto seg = open();
  ASSERT_NE(seg, nullptr);

  builder_.clear();
  ASSERT_TRUE(builder_.add_record(Slice::from_string("x")).ok());
  const Slice framed = builder_.finish(7, BatchMeta{});  // segment is at offset 0
  auto header = storage::decode_header(framed);
  ASSERT_TRUE(header.ok());

  auto st = seg->append(framed, header.value());
  ASSERT_FALSE(st.ok());
  EXPECT_EQ(st.error(), base::ErrorCode::kInvalidArgument);
}

// FR-7: a torn write at the tail is normal. The bytes that survived are kept, the
// partial batch is dropped, and the segment comes back usable.
TEST_F(SegmentTest, RecoversFromATornTail) {
  base::u64 two_batch_bytes = 0;
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    append_batch(*seg, 2, 0);
    append_batch(*seg, 2, 2);
    two_batch_bytes = file_size(log_path());
    append_batch(*seg, 2, 4);
    ASSERT_TRUE(seg->fsync().ok());
    // Note: no flush_index(). This is the kill -9 case — the index never made it out.
  }

  // Chop the third batch in half.
  const base::u64 torn_at = two_batch_bytes + 10;
  std::filesystem::resize_file(log_path(), torn_at);

  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(seg->next_offset(), 4u);
  EXPECT_TRUE(seg->recovery().truncated);
  EXPECT_EQ(seg->recovery().bytes_truncated, torn_at - two_batch_bytes);
  EXPECT_EQ(seg->recovery().batches_scanned, 2u);
  EXPECT_EQ(file_size(log_path()), two_batch_bytes);

  // And it is still appendable, at the right offset.
  EXPECT_EQ(append_batch(*seg, 1, 100), 4u);
}

TEST_F(SegmentTest, RecoveryStopsAtACorruptedBatch) {
  base::u64 first_batch_bytes = 0;
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    append_batch(*seg, 2, 0);
    first_batch_bytes = file_size(log_path());
    append_batch(*seg, 2, 2);
    append_batch(*seg, 2, 4);
    ASSERT_TRUE(seg->fsync().ok());
  }

  // Flip a byte inside the second batch's payload — a silent corruption, not a torn
  // write. The CRC is the only thing that can tell.
  poke(log_path(), first_batch_bytes + storage::kBatchHeaderBytes + 2, '!');

  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(seg->next_offset(), 2u);
  EXPECT_TRUE(seg->recovery().truncated);
  EXPECT_EQ(seg->recovery().batches_scanned, 1u);
}

// The CRC covers everything after itself, which includes base_offset's *successor*
// fields but not base_offset. Divergence at the offset level is caught by the
// continuity check instead — the reason recovery tracks an expected offset at all.
TEST_F(SegmentTest, RecoveryStopsAtABatchWithTheWrongBaseOffset) {
  base::u64 first_batch_bytes = 0;
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    append_batch(*seg, 2, 0);
    first_batch_bytes = file_size(log_path());
    append_batch(*seg, 2, 2);
    ASSERT_TRUE(seg->fsync().ok());
  }

  poke(log_path(), first_batch_bytes, static_cast<char>(9));  // base_offset 2 → 9

  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(seg->next_offset(), 2u);
  EXPECT_TRUE(seg->recovery().truncated);
}

// Bug journal #1. A read that *fails* and a read that comes up *short* mean opposite
// things: short is end-of-file and the tail really is gone, but an I/O error is the disk
// declining to answer and says nothing about the bytes. Recovery originally treated them
// alike and truncated on both, deleting durable acked records because one read hiccuped.
TEST_F(SegmentTest, ATransientReadErrorDuringRecoveryNeverTruncates) {
  base::u64 full_size = 0;
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    for (base::u64 i = 0; i < 12; ++i) append_batch(*seg, 2, i * 2);
    ASSERT_TRUE(seg->fsync().ok());
    full_size = file_size(log_path());
  }

  // Fail a read partway through the scan, not the very first one, so recovery has
  // already accepted some batches and has something to destroy.
  for (int fail_at = 0; fail_at < 8; ++fail_at) {
    FlakyDisk flaky(disk_, fail_at);
    auto seg = Segment::open(flaky, dir(), 0, options());

    ASSERT_FALSE(seg.ok()) << "recovery continued past a failed read (fail_at=" << fail_at << ")";
    EXPECT_EQ(seg.error(), base::ErrorCode::kIoError) << "fail_at=" << fail_at;
    EXPECT_EQ(flaky.truncates(), 0) << "truncated on an I/O error (fail_at=" << fail_at << ")";
    EXPECT_EQ(file_size(log_path()), full_size)
        << "durable bytes disappeared on a transient error (fail_at=" << fail_at << ")";
  }

  // And once the disk behaves, everything is still there.
  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_EQ(seg->next_offset(), 24u);
  EXPECT_FALSE(seg->recovery().truncated);
}

// The same distinction one level down. A client told CORRUPT_RECORD — a terminal error —
// gives up for good on data that was never damaged.
TEST_F(SegmentTest, ATransientReadErrorOnFetchIsNotReportedAsCorruption) {
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    append_batch(*seg, 2, 0);
    append_batch(*seg, 2, 2);
    ASSERT_TRUE(seg->fsync().ok());
  }

  // Open cleanly, so recovery is behind us and the next pread belongs to the read path.
  FlakyDisk flaky(disk_, -1);
  auto opened = Segment::open(flaky, dir(), 0, options());
  ASSERT_TRUE(opened.ok());
  std::unique_ptr<Segment> segment = std::move(opened).value();
  ASSERT_EQ(segment->next_offset(), 4u);

  std::vector<base::u8> buf(4096);
  ASSERT_TRUE(segment->read(0, MutSlice(buf.data(), buf.size())).ok());

  flaky.fail_next_pread();
  auto got = segment->read(0, MutSlice(buf.data(), buf.size()));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error(), base::ErrorCode::kIoError)
      << "an I/O error must not masquerade as CORRUPT_RECORD — one is retryable and the "
         "other tells the client to give up forever";

  // The disk recovers; so does the read.
  EXPECT_TRUE(segment->read(0, MutSlice(buf.data(), buf.size())).ok());
}

TEST_F(SegmentTest, AGarbageIndexCostsAScanAndNothingElse) {
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    append_batch(*seg, 2, 0);
    append_batch(*seg, 2, 2);
    ASSERT_TRUE(seg->fsync().ok());
  }

  // Plausible-looking entries pointing into the middle of nowhere. The index is never
  // fsynced, so this is not a hypothetical: it is what a crash can leave behind.
  {
    std::ofstream f(index_path(), std::ios::binary | std::ios::trunc);
    const unsigned char bytes[] = {0, 0, 0, 0, 7, 0, 0, 0, 1, 0, 0, 0, 30, 0, 0, 0};
    f.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
  }

  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_FALSE(seg->recovery().index_was_usable);
  EXPECT_FALSE(seg->recovery().truncated);
  EXPECT_EQ(seg->next_offset(), 4u);
  EXPECT_EQ(seg->recovery().batches_scanned, 2u);
}

TEST_F(SegmentTest, ACleanCloseLetsRecoverySkipMostOfTheScan) {
  index_interval_ = 64;  // one entry every 64 bytes, so the index has real content
  {
    auto seg = open();
    ASSERT_NE(seg, nullptr);
    for (base::u64 i = 0; i < 20; ++i) append_batch(*seg, 1, i);
    ASSERT_TRUE(seg->fsync().ok());
    ASSERT_TRUE(seg->flush_index().ok());
  }

  auto seg = open();
  ASSERT_NE(seg, nullptr);
  EXPECT_TRUE(seg->recovery().index_was_usable);
  EXPECT_EQ(seg->next_offset(), 20u);
  // The scan resumed from the last index entry rather than from byte zero. That is
  // the entire value of the index on the recovery path.
  EXPECT_LT(seg->recovery().batches_scanned, 20u);
}

}  // namespace
