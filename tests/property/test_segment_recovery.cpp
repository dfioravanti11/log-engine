#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "io/real/real_disk.h"
#include "io/seeded_random.h"
#include "storage/segment.h"
#include "support/temp_dir.h"

// Recovery is the one week-2 property worth testing exhaustively: the interesting
// inputs are "a segment cut off at byte N", for every N, and no hand-written case list
// covers those the way a seeded loop does.
//
// The property, stated once: **after recovery a segment holds exactly the longest
// prefix of whole, valid batches.** Not "most of the data", not "the data minus a bit"
// — a prefix, because anything else means an offset that is readable now and might not
// be later, which is the un-read a log service may never do.
//
// Every failure prints its seed. A lost seed is a lost bug (CLAUDE.md rule 5).
namespace {

using base::MutSlice;
using base::Slice;
using storage::BatchBuilder;
using storage::BatchMeta;
using storage::Segment;

constexpr base::u64 kSeed = 0x5EED'0002;

struct WrittenBatch {
  base::u64 base_offset = 0;
  base::u64 next_offset = 0;
  base::u64 end_pos = 0;  // byte position just past this batch
};

Segment::Options segment_options() {
  Segment::Options o;
  o.max_bytes = 4u << 20;
  o.index_interval_bytes = 128;
  return o;
}

// Writes a random run of batches into `dir` and returns what went in.
std::vector<WrittenBatch> write_log(io::Disk& disk, const std::string& dir,
                                    io::SeededRandom& rng, int batch_count) {
  std::vector<WrittenBatch> written;
  auto opened = Segment::open(disk, dir, 0, segment_options());
  EXPECT_TRUE(opened.ok());
  if (!opened.ok()) return written;
  std::unique_ptr<Segment> segment = std::move(opened).value();

  BatchBuilder builder;
  for (int i = 0; i < batch_count; ++i) {
    builder.clear();
    const auto records = static_cast<base::u32>(1 + rng.next_below(8));
    for (base::u32 r = 0; r < records; ++r) {
      const std::string payload(1 + rng.next_below(200), static_cast<char>('a' + (i % 26)));
      EXPECT_TRUE(builder.add_record(Slice::from_string(payload)).ok());
    }

    const base::u64 base_offset = segment->next_offset();
    const Slice framed = builder.finish(base_offset, BatchMeta{});
    auto header = storage::decode_header(framed);
    EXPECT_TRUE(header.ok());
    EXPECT_TRUE(segment->append(framed, header.value()).ok());

    written.push_back(
        WrittenBatch{base_offset, header.value().next_offset(), segment->size_bytes()});
  }
  EXPECT_TRUE(segment->fsync().ok());
  // Deliberately no flush_index(): the crash case is the interesting one, and it is
  // the case where the index never reached the disk at all.
  return written;
}

// Reads every batch back and checks it is intact and contiguous from offset 0.
void verify_readable(Segment& segment, base::u64 seed) {
  std::vector<base::u8> buf(1 << 16);
  base::u64 offset = 0;
  while (offset < segment.next_offset()) {
    auto got = segment.read(offset, MutSlice(buf.data(), buf.size()));
    ASSERT_TRUE(got.ok()) << "seed=" << seed;
    ASSERT_GT(got.value(), 0u) << "seed=" << seed;

    std::size_t pos = 0;
    while (pos < got.value()) {
      const Slice framed = Slice(buf.data(), got.value()).subslice(pos);
      ASSERT_TRUE(storage::validate_batch(framed).ok()) << "seed=" << seed;
      auto header = storage::decode_header(framed);
      ASSERT_TRUE(header.ok()) << "seed=" << seed;
      ASSERT_EQ(header.value().base_offset, offset) << "seed=" << seed;
      offset = header.value().next_offset();
      pos += header.value().total_bytes();
    }
  }
  EXPECT_EQ(offset, segment.next_offset()) << "seed=" << seed;
}

class RecoveryProperty : public testsupport::TempDirTest {
 protected:
  // One subdirectory per trial, so a failure leaves its own files behind and the
  // trials cannot contaminate each other.
  std::string trial_dir(int trial) {
    const std::string d = path("trial_" + std::to_string(trial));
    std::filesystem::create_directories(d);
    return d;
  }

  io::real::RealDisk disk_;
};

TEST_F(RecoveryProperty, TruncationAtAnyByteRecoversTheLongestValidPrefix) {
  io::SeededRandom picker(kSeed);

  for (int trial = 0; trial < 100; ++trial) {
    const base::u64 seed = kSeed + static_cast<base::u64>(trial);
    const std::string dir = trial_dir(trial);
    const std::string log = dir + "/" + Segment::log_name(0);

    io::SeededRandom rng(seed);
    const auto written = write_log(disk_, dir, rng, 1 + static_cast<int>(picker.next_below(20)));
    ASSERT_FALSE(written.empty()) << "seed=" << seed;

    const base::u64 full_size = written.back().end_pos;
    const base::u64 cut_to = picker.next_below(full_size + 1);
    std::filesystem::resize_file(log, cut_to);

    // What *should* survive: every batch that fits entirely below the cut.
    base::u64 expect_next_offset = 0;
    base::u64 expect_bytes = 0;
    for (const WrittenBatch& b : written) {
      if (b.end_pos > cut_to) break;
      expect_next_offset = b.next_offset;
      expect_bytes = b.end_pos;
    }

    auto reopened = Segment::open(disk_, dir, 0, segment_options());
    ASSERT_TRUE(reopened.ok()) << "seed=" << seed;
    std::unique_ptr<Segment> segment = std::move(reopened).value();

    EXPECT_EQ(segment->next_offset(), expect_next_offset) << "seed=" << seed << " cut=" << cut_to;
    EXPECT_EQ(segment->size_bytes(), expect_bytes) << "seed=" << seed << " cut=" << cut_to;
    // A cut that happens to land exactly on a batch boundary leaves a clean prefix
    // with nothing to throw away — the common intuition that "truncated file" implies
    // "recovery truncated something" is wrong, and this is the case that says so.
    EXPECT_EQ(segment->recovery().truncated, expect_bytes != cut_to) << "seed=" << seed;
    EXPECT_EQ(segment->recovery().bytes_truncated, cut_to - expect_bytes) << "seed=" << seed;
    verify_readable(*segment, seed);
  }
}

// Silent corruption rather than truncation. A flipped bit anywhere in a batch must
// stop recovery at or before that batch — never after it, which would mean serving a
// record whose bytes are not the bytes that were written.
TEST_F(RecoveryProperty, CorruptionStopsRecoveryAtOrBeforeTheDamagedBatch) {
  constexpr base::u64 kCorruptSeed = kSeed ^ 0xC0FFEE;
  io::SeededRandom picker(kCorruptSeed);

  for (int trial = 0; trial < 100; ++trial) {
    const base::u64 seed = kCorruptSeed + static_cast<base::u64>(trial);
    const std::string dir = trial_dir(1000 + trial);
    const std::string log = dir + "/" + Segment::log_name(0);

    io::SeededRandom rng(seed);
    const auto written = write_log(disk_, dir, rng, 2 + static_cast<int>(picker.next_below(15)));
    ASSERT_FALSE(written.empty()) << "seed=" << seed;

    const auto victim = static_cast<std::size_t>(picker.next_below(written.size()));
    const base::u64 start = victim == 0 ? 0 : written[victim - 1].end_pos;
    const base::u64 at = start + picker.next_below(written[victim].end_pos - start);

    {
      std::fstream f(log, std::ios::in | std::ios::out | std::ios::binary);
      f.seekg(static_cast<std::streamoff>(at));
      char original = 0;
      f.read(&original, 1);
      f.seekp(static_cast<std::streamoff>(at));
      f.put(static_cast<char>(original ^ 0x40));
    }

    auto reopened = Segment::open(disk_, dir, 0, segment_options());
    ASSERT_TRUE(reopened.ok()) << "seed=" << seed;
    std::unique_ptr<Segment> segment = std::move(reopened).value();

    // "At or before", not "exactly at": a flip in base_offset is caught by the offset
    // continuity check and a flip in batch_length by the length check, both of which
    // fire before the CRC ever runs. What matters is that no path lets the damaged
    // batch through.
    EXPECT_LE(segment->next_offset(), written[victim].base_offset)
        << "seed=" << seed << " corrupted byte " << at << " of batch " << victim;
    verify_readable(*segment, seed);
  }
}

}  // namespace
