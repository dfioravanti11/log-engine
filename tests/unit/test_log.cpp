#include "storage/log.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "io/real/real_disk.h"
#include "support/temp_dir.h"

namespace {

using base::MutSlice;
using base::Slice;
using storage::BatchBuilder;
using storage::BatchMeta;
using storage::Log;
using testsupport::record_payload;

class LogTest : public testsupport::TempDirTest {
 protected:
  Log::Options options() const {
    Log::Options o;
    o.segment_max_bytes = segment_max_bytes_;
    return o;
  }

  std::unique_ptr<Log> open() {
    auto log = Log::open(disk_, dir(), options());
    EXPECT_TRUE(log.ok()) << base::to_string(log.error());
    return log.ok() ? std::move(log).value() : nullptr;
  }

  base::u64 append_batch(Log& log, base::u32 records, base::u64 first_record_id) {
    builder_.clear();
    for (base::u32 i = 0; i < records; ++i) {
      const std::string payload = record_payload(first_record_id + i);
      EXPECT_TRUE(builder_.add_record(Slice::from_string(payload)).ok());
    }
    auto base_offset = log.append(builder_, BatchMeta{});
    EXPECT_TRUE(base_offset.ok()) << base::to_string(base_offset.error());
    return base_offset.ok() ? base_offset.value() : 0;
  }

  // Reads the whole log back and returns every record payload, in order.
  std::vector<std::string> read_all(const Log& log) {
    std::vector<std::string> out;
    std::vector<base::u8> buf(64 * 1024);
    base::u64 offset = log.start_offset();

    while (offset < log.next_offset()) {
      auto got = log.read(offset, MutSlice(buf.data(), buf.size()));
      EXPECT_TRUE(got.ok()) << base::to_string(got.error());
      if (!got.ok() || got.value() == 0) break;

      std::size_t pos = 0;
      while (pos < got.value()) {
        const Slice framed = Slice(buf.data(), got.value()).subslice(pos);
        EXPECT_TRUE(storage::validate_batch(framed).ok());
        auto header = storage::decode_header(framed);
        EXPECT_TRUE(header.ok());
        if (!header.ok()) return out;

        storage::RecordIterator it(framed);
        Slice value;
        while (it.next(&value)) out.emplace_back(value.as_string_view());

        pos += header.value().total_bytes();
        offset = header.value().next_offset();
      }
    }
    return out;
  }

  std::vector<std::string> expected_records(base::u64 count) const {
    std::vector<std::string> out;
    for (base::u64 i = 0; i < count; ++i) out.push_back(record_payload(i));
    return out;
  }

  // Frames a batch at a caller-chosen offset without appending it — this is what a
  // leader's bytes look like by the time they reach a follower.
  Slice frame_at(base::u64 base_offset, base::u32 records, base::u64 first_record_id,
                 base::u32 leader_epoch) {
    framer_.clear();
    for (base::u32 i = 0; i < records; ++i) {
      const std::string payload = record_payload(first_record_id + i);
      EXPECT_TRUE(framer_.add_record(Slice::from_string(payload)).ok());
    }
    BatchMeta meta;
    meta.leader_epoch = leader_epoch;
    return framer_.finish(base_offset, meta);
  }

  io::real::RealDisk disk_;
  BatchBuilder builder_;
  BatchBuilder framer_;
  base::u32 segment_max_bytes_ = 1u << 20;
};

TEST_F(LogTest, EmptyDirectoryStartsAtOffsetZero) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  EXPECT_EQ(log->start_offset(), 0u);
  EXPECT_EQ(log->next_offset(), 0u);
  EXPECT_EQ(log->segment_count(), 1u);
}

TEST_F(LogTest, OffsetsAdvanceByRecordCount) {
  auto log = open();
  ASSERT_NE(log, nullptr);

  EXPECT_EQ(append_batch(*log, 3, 0), 0u);
  EXPECT_EQ(append_batch(*log, 1, 3), 3u);
  EXPECT_EQ(append_batch(*log, 5, 4), 4u);
  EXPECT_EQ(log->next_offset(), 9u);

  EXPECT_EQ(read_all(*log), expected_records(9));
}

TEST_F(LogTest, RollsToANewSegmentAtTheSizeBound) {
  segment_max_bytes_ = 4096;
  auto log = open();
  ASSERT_NE(log, nullptr);

  for (base::u64 i = 0; i < 200; ++i) append_batch(*log, 1, i);
  ASSERT_GT(log->segment_count(), 1u);

  // Every segment starts exactly where the previous one ended. A gap here would mean
  // an offset that no sequential read can ever reach.
  for (std::size_t i = 1; i < log->segment_count(); ++i) {
    EXPECT_EQ(log->segment(i).base_offset(), log->segment(i - 1).next_offset());
  }
  EXPECT_EQ(read_all(*log), expected_records(200));
}

TEST_F(LogTest, RejectsABatchLargerThanASegment) {
  segment_max_bytes_ = 4096;
  auto log = open();
  ASSERT_NE(log, nullptr);

  builder_.clear();
  const std::string big(8192, 'x');
  ASSERT_TRUE(builder_.add_record(Slice::from_string(big)).ok());

  auto result = log->append(builder_, BatchMeta{});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error(), base::ErrorCode::kMessageTooLarge);
}

TEST_F(LogTest, ReopenPreservesEverythingThatWasFsynced) {
  segment_max_bytes_ = 4096;
  {
    auto log = open();
    ASSERT_NE(log, nullptr);
    for (base::u64 i = 0; i < 100; ++i) append_batch(*log, 1, i);
    ASSERT_TRUE(log->fsync().ok());
  }

  auto log = open();
  ASSERT_NE(log, nullptr);
  EXPECT_EQ(log->next_offset(), 100u);
  EXPECT_FALSE(log->recovery().truncated);
  EXPECT_EQ(read_all(*log), expected_records(100));

  // And the reopened log keeps counting from where it left off.
  EXPECT_EQ(append_batch(*log, 1, 100), 100u);
}

TEST_F(LogTest, ReopenRecoversATornActiveSegment) {
  segment_max_bytes_ = 4096;
  {
    auto log = open();
    ASSERT_NE(log, nullptr);
    for (base::u64 i = 0; i < 100; ++i) append_batch(*log, 1, i);
    ASSERT_TRUE(log->fsync().ok());
  }

  // Chop three bytes off the newest segment, as a half-finished pwrite would.
  base::u64 last_base = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir_)) {
    const std::string name = entry.path().filename().string();
    if (auto parsed = storage::Segment::parse_log_name(name); parsed) {
      last_base = std::max(last_base, parsed.value());
    }
  }
  const std::string victim = path(storage::Segment::log_name(last_base));
  std::filesystem::resize_file(victim, std::filesystem::file_size(victim) - 3);

  auto log = open();
  ASSERT_NE(log, nullptr);
  EXPECT_TRUE(log->recovery().truncated);
  EXPECT_LT(log->next_offset(), 100u);

  // Whatever survived is a valid prefix — not "most of" the log, exactly a prefix.
  const auto records = read_all(*log);
  EXPECT_EQ(records, expected_records(log->next_offset()));
}

// A truncation in the middle of the log leaves every later segment unreachable: the
// offsets after the hole can never be read sequentially. Discarding them is the only
// honest option, and it is the case a crash during a roll produces.
TEST_F(LogTest, DiscardsSegmentsStrandedBehindAHole) {
  segment_max_bytes_ = 4096;
  base::u64 total_segments = 0;
  {
    auto log = open();
    ASSERT_NE(log, nullptr);
    for (base::u64 i = 0; i < 200; ++i) append_batch(*log, 1, i);
    ASSERT_TRUE(log->fsync().ok());
    total_segments = log->segment_count();
  }
  ASSERT_GT(total_segments, 2u);

  // Tear the *first* segment, not the last.
  const std::string victim = path(storage::Segment::log_name(0));
  std::filesystem::resize_file(victim, std::filesystem::file_size(victim) - 3);

  auto log = open();
  ASSERT_NE(log, nullptr);
  EXPECT_EQ(log->segment_count(), 1u);
  EXPECT_EQ(log->recovery().segments_discarded, total_segments - 1);
  EXPECT_EQ(read_all(*log), expected_records(log->next_offset()));
}

TEST_F(LogTest, ReadingPastTheEndReturnsNothing) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  append_batch(*log, 4, 0);

  std::vector<base::u8> buf(4096);
  auto got = log->read(log->next_offset(), MutSlice(buf.data(), buf.size()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), 0u);
}

// ---- The replication path (week 5) ----

TEST_F(LogTest, AReplicatedBatchLandsAtTheOffsetItsHeaderNames) {
  auto log = open();
  ASSERT_NE(log, nullptr);

  ASSERT_TRUE(log->append_replicated(frame_at(0, 3, 0, 7)).ok());
  ASSERT_TRUE(log->append_replicated(frame_at(3, 2, 3, 7)).ok());

  EXPECT_EQ(log->next_offset(), 5u);
  EXPECT_EQ(read_all(*log), expected_records(5));
}

// `Log` is the offset authority for everything a client writes (I2). On this one path the
// authority sits upstream in the leader, and the check that it stayed consistent is that
// the batch's own offset must continue our log exactly.
TEST_F(LogTest, AReplicatedBatchAtTheWrongOffsetIsRejected) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  ASSERT_TRUE(log->append_replicated(frame_at(0, 3, 0, 1)).ok());

  // A gap.
  auto gap = log->append_replicated(frame_at(4, 1, 4, 1));
  EXPECT_FALSE(gap.ok());
  EXPECT_EQ(gap.error(), base::ErrorCode::kInvalidArgument);

  // An overlap.
  auto overlap = log->append_replicated(frame_at(2, 1, 2, 1));
  EXPECT_FALSE(overlap.ok());
  EXPECT_EQ(overlap.error(), base::ErrorCode::kInvalidArgument);

  EXPECT_EQ(log->next_offset(), 3u) << "a rejected batch must not move the log";
}

// These bytes came off a network, from a peer that may have read them off a bad disk.
// They are checked before they are written, not after they are read back.
TEST_F(LogTest, AReplicatedBatchWithACorruptedBodyIsRejected) {
  auto log = open();
  ASSERT_NE(log, nullptr);

  const Slice good = frame_at(0, 3, 0, 1);
  std::vector<base::u8> damaged(good.data(), good.data() + good.size());
  damaged[good.size() - 1] ^= 0x40u;

  auto result = log->append_replicated(Slice(damaged.data(), damaged.size()));
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error(), base::ErrorCode::kCorruptRecord);
  EXPECT_EQ(log->next_offset(), 0u);
}

TEST_F(LogTest, TruncateDropsEverythingFromTheOffset) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  append_batch(*log, 2, 0);  // offsets 0-1
  append_batch(*log, 2, 2);  // offsets 2-3
  append_batch(*log, 2, 4);  // offsets 4-5
  ASSERT_EQ(log->next_offset(), 6u);

  ASSERT_TRUE(log->truncate_to(2).ok());
  EXPECT_EQ(log->next_offset(), 2u);
  EXPECT_EQ(read_all(*log), expected_records(2));

  // And the log keeps working from the cut, with the offsets continuing correctly.
  EXPECT_EQ(append_batch(*log, 2, 2), 2u);
  EXPECT_EQ(log->next_offset(), 4u);
  EXPECT_EQ(read_all(*log), expected_records(4));
}

TEST_F(LogTest, TruncateSpanningSegmentsRemovesTheOnesEntirelyPastTheCut) {
  segment_max_bytes_ = 512;
  auto log = open();
  ASSERT_NE(log, nullptr);
  for (base::u64 i = 0; i < 20; ++i) append_batch(*log, 1, i);
  ASSERT_GT(log->segment_count(), 2u) << "this test needs several segments to be about anything";
  ASSERT_EQ(log->next_offset(), 20u);

  ASSERT_TRUE(log->truncate_to(3).ok());
  EXPECT_EQ(log->next_offset(), 3u);
  EXPECT_EQ(read_all(*log), expected_records(3));
  EXPECT_EQ(log->segment_count(), 1u);
}

TEST_F(LogTest, TruncateSurvivesAReopen) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  append_batch(*log, 2, 0);
  append_batch(*log, 2, 2);
  ASSERT_TRUE(log->truncate_to(2).ok());
  ASSERT_TRUE(log->fsync().ok());
  log.reset();

  auto reopened = open();
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(reopened->next_offset(), 2u);
  EXPECT_EQ(read_all(*reopened), expected_records(2));
}

TEST_F(LogTest, TruncateToTheEndIsANoOpAndOutOfRangeIsAnError) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  append_batch(*log, 4, 0);

  EXPECT_TRUE(log->truncate_to(4).ok());
  EXPECT_EQ(log->next_offset(), 4u);

  auto past = log->truncate_to(9);
  EXPECT_FALSE(past.ok());
  EXPECT_EQ(past.error(), base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(log->next_offset(), 4u);
}

TEST_F(LogTest, ScanEpochsCollapsesConsecutiveBatchesFromOneLeader) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  // Five batches from epoch 3, then three from epoch 8, then one back at... no: epochs
  // only ever increase, because a term never comes back.
  for (base::u64 i = 0; i < 5; ++i) {
    ASSERT_TRUE(log->append_replicated(frame_at(i, 1, i, 3)).ok());
  }
  for (base::u64 i = 5; i < 8; ++i) {
    ASSERT_TRUE(log->append_replicated(frame_at(i, 1, i, 8)).ok());
  }

  auto epochs = log->scan_epochs();
  ASSERT_TRUE(epochs.ok()) << base::to_string(epochs.error());
  ASSERT_EQ(epochs.value().size(), 2u) << "one entry per change, not per batch";
  EXPECT_EQ(epochs.value()[0].epoch, 3u);
  EXPECT_EQ(epochs.value()[0].start_offset, 0u);
  EXPECT_EQ(epochs.value()[1].epoch, 8u);
  EXPECT_EQ(epochs.value()[1].start_offset, 5u);
}

TEST_F(LogTest, ScanEpochsSpansSegmentsAndSurvivesAReopen) {
  segment_max_bytes_ = 512;
  auto log = open();
  ASSERT_NE(log, nullptr);
  for (base::u64 i = 0; i < 30; ++i) {
    ASSERT_TRUE(log->append_replicated(frame_at(i, 1, i, i < 15 ? 2u : 6u)).ok());
  }
  ASSERT_GT(log->segment_count(), 1u);
  ASSERT_TRUE(log->fsync().ok());
  log.reset();

  auto reopened = open();
  ASSERT_NE(reopened, nullptr);
  auto epochs = reopened->scan_epochs();
  ASSERT_TRUE(epochs.ok());
  ASSERT_EQ(epochs.value().size(), 2u);
  EXPECT_EQ(epochs.value()[0].epoch, 2u);
  EXPECT_EQ(epochs.value()[0].start_offset, 0u);
  EXPECT_EQ(epochs.value()[1].epoch, 6u);
  EXPECT_EQ(epochs.value()[1].start_offset, 15u);
}

TEST_F(LogTest, AnEmptyLogHasNoEpochs) {
  auto log = open();
  ASSERT_NE(log, nullptr);
  auto epochs = log->scan_epochs();
  ASSERT_TRUE(epochs.ok());
  EXPECT_TRUE(epochs.value().empty());
}

}  // namespace
