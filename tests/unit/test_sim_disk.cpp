#include "io/sim/sim_disk.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "io/seeded_random.h"

namespace {

using base::MutSlice;
using base::Slice;
using io::OpenMode;
using io::sim::DiskFaultConfig;
using io::sim::SimDisk;

constexpr base::u64 kSeed = 0x5EEDC0DEull;

std::string seed_note(base::u64 seed) {
  std::ostringstream os;
  os << "seed=0x" << std::hex << seed;
  return os.str();
}

// Reopening is what a restarted process would have to do: crash() invalidates every
// FileId, so no test may read through a handle it held before the power cut.
std::vector<base::u8> read_path(SimDisk& disk, const std::string& path) {
  auto file = disk.open(path, OpenMode::kRead);
  if (!file) return {};
  auto bytes = disk.size(file.value());
  std::vector<base::u8> out(bytes.ok() ? static_cast<std::size_t>(bytes.value()) : 0);
  if (!out.empty()) {
    auto got = disk.pread(file.value(), MutSlice(out.data(), out.size()), 0);
    out.resize(got.ok() ? got.value() : 0);
  }
  disk.close(file.value());
  return out;
}

void pwrite_fill(SimDisk& disk, io::FileId file, base::u8 value, std::size_t count,
                 base::u64 offset) {
  const std::vector<base::u8> buf(count, value);
  auto written = disk.pwrite(file, Slice(buf.data(), buf.size()), offset);
  ASSERT_TRUE(written.ok());
  ASSERT_EQ(written.value(), count);
}

class SimDiskTest : public ::testing::Test {
 protected:
  // CLAUDE.md rule 5: a lost seed is a lost bug. Heap-allocating the trace here gives
  // it test-long scope, so every failure below carries the seed without each
  // assertion having to say so.
  void SetUp() override {
    trace_ = std::make_unique<::testing::ScopedTrace>(__FILE__, __LINE__, seed_note(kSeed));
  }

  std::unique_ptr<::testing::ScopedTrace> trace_;
  io::SeededRandom rng_{kSeed};
  SimDisk disk_{rng_};
};

// ---------------------------------------------------------------------------
// Parity with RealDisk. Every case here has a twin in test_disk.cpp; storage/ can
// only be tested against the simulator if the two disks are indistinguishable from
// above the seam.
// ---------------------------------------------------------------------------

TEST_F(SimDiskTest, WriteReadRoundTrip) {
  auto file = disk_.open("a.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());

  const std::string data = "segment bytes";
  auto written = disk_.pwrite(file.value(), Slice::from_string(data), 0);
  ASSERT_TRUE(written.ok());
  EXPECT_EQ(written.value(), data.size());

  std::vector<base::u8> out(data.size());
  auto got = disk_.pread(file.value(), MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), data.size());
  EXPECT_EQ(Slice(out.data(), out.size()), Slice::from_string(data));
}

TEST_F(SimDiskTest, PositionalWritesDoNotDisturbEachOther) {
  auto file = disk_.open("b.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk_.pwrite(file.value(), Slice::from_string("BBBB"), 4).ok());
  ASSERT_TRUE(disk_.pwrite(file.value(), Slice::from_string("AAAA"), 0).ok());

  std::vector<base::u8> out(8);
  auto got = disk_.pread(file.value(), MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(Slice(out.data(), 8), Slice::from_string("AAAABBBB"));
}

// Recovery's tail scan walks off the end of a torn segment on purpose (§16.2).
TEST_F(SimDiskTest, ShortReadAtEofIsNotAnError) {
  auto file = disk_.open("c.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk_.pwrite(file.value(), Slice::from_string("abc"), 0).ok());

  std::vector<base::u8> out(64);
  auto got = disk_.pread(file.value(), MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), 3u);

  auto past = disk_.pread(file.value(), MutSlice(out.data(), out.size()), 100);
  ASSERT_TRUE(past.ok());
  EXPECT_EQ(past.value(), 0u);
}

TEST_F(SimDiskTest, PwritePastEofZeroFillsTheGap) {
  auto file = disk_.open("gap.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk_.pwrite(file.value(), Slice::from_string("XY"), 4).ok());

  auto bytes = disk_.size(file.value());
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value(), 6u);

  std::vector<base::u8> out(6);
  auto got = disk_.pread(file.value(), MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  ASSERT_EQ(got.value(), 6u);
  const std::vector<base::u8> want = {0, 0, 0, 0, 'X', 'Y'};
  EXPECT_EQ(out, want);
}

TEST_F(SimDiskTest, SizeAndTruncateBothDirections) {
  auto file = disk_.open("d.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk_.pwrite(file.value(), Slice::from_string("0123456789"), 0).ok());

  auto bytes = disk_.size(file.value());
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value(), 10u);

  ASSERT_TRUE(disk_.truncate(file.value(), 4).ok());
  bytes = disk_.size(file.value());
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value(), 4u);

  ASSERT_TRUE(disk_.truncate(file.value(), 8).ok());
  std::vector<base::u8> out(8);
  auto got = disk_.pread(file.value(), MutSlice(out.data(), out.size()), 0);
  ASSERT_TRUE(got.ok());
  ASSERT_EQ(got.value(), 8u);
  const std::vector<base::u8> want = {'0', '1', '2', '3', 0, 0, 0, 0};
  EXPECT_EQ(out, want);
}

TEST_F(SimDiskTest, OpenMissingFileIsNotFound) {
  auto read = disk_.open("nope.log", OpenMode::kRead);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error(), base::ErrorCode::kNotFound);

  auto rw = disk_.open("nope.log", OpenMode::kReadWrite);
  ASSERT_FALSE(rw.ok());
  EXPECT_EQ(rw.error(), base::ErrorCode::kNotFound);
}

TEST_F(SimDiskTest, CreateDoesNotTruncateAndReadOnlyRejectsWrites) {
  auto file = disk_.open("keep.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk_.pwrite(file.value(), Slice::from_string("abc"), 0).ok());
  disk_.close(file.value());

  auto again = disk_.open("keep.log", OpenMode::kCreate);
  ASSERT_TRUE(again.ok());
  auto bytes = disk_.size(again.value());
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value(), 3u);
  disk_.close(again.value());

  auto ro = disk_.open("keep.log", OpenMode::kRead);
  ASSERT_TRUE(ro.ok());
  auto written = disk_.pwrite(ro.value(), Slice::from_string("x"), 0);
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error(), base::ErrorCode::kIoError);
  EXPECT_EQ(disk_.truncate(ro.value(), 0).error(), base::ErrorCode::kIoError);
}

// O_CREAT does not create the parent directory, and a Log that skipped
// make_directories() should fail here rather than in a later recovery scan.
TEST_F(SimDiskTest, CreateNeedsAnExistingParentDirectory) {
  auto missing = disk_.open("data/topic-0/x.log", OpenMode::kCreate);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error(), base::ErrorCode::kNotFound);

  ASSERT_TRUE(disk_.make_directories("data/topic-0").ok());
  EXPECT_TRUE(disk_.make_directories("data/topic-0").ok());  // idempotent
  EXPECT_TRUE(disk_.open("data/topic-0/x.log", OpenMode::kCreate).ok());
}

TEST_F(SimDiskTest, RemoveDeletesTheFile) {
  ASSERT_TRUE(disk_.open("r.log", OpenMode::kCreate).ok());
  EXPECT_EQ(disk_.file_count(), 1u);

  EXPECT_TRUE(disk_.remove("r.log").ok());
  EXPECT_EQ(disk_.file_count(), 0u);
  EXPECT_EQ(disk_.remove("r.log").error(), base::ErrorCode::kNotFound);
  EXPECT_EQ(disk_.open("r.log", OpenMode::kRead).error(), base::ErrorCode::kNotFound);
}

TEST_F(SimDiskTest, ListDirectoryIsSortedAndScopedToOneLevel) {
  ASSERT_TRUE(disk_.make_directories("data/topic-0/nested").ok());
  for (const char* name : {"00000000000000000010.log", "00000000000000000000.log",
                           "00000000000000000000.index"}) {
    ASSERT_TRUE(disk_.open(std::string("data/topic-0/") + name, OpenMode::kCreate).ok());
  }
  ASSERT_TRUE(disk_.open("data/stray.log", OpenMode::kCreate).ok());

  auto names = disk_.list_directory("data/topic-0");
  ASSERT_TRUE(names.ok());
  const std::vector<std::string> want = {"00000000000000000000.index",
                                         "00000000000000000000.log",
                                         "00000000000000000010.log", "nested"};
  EXPECT_EQ(names.value(), want);

  // A trailing slash names the same directory; storage/ joins paths by hand.
  auto with_slash = disk_.list_directory("data/topic-0/");
  ASSERT_TRUE(with_slash.ok());
  EXPECT_EQ(with_slash.value(), want);

  auto parent = disk_.list_directory("data");
  ASSERT_TRUE(parent.ok());
  const std::vector<std::string> want_parent = {"stray.log", "topic-0"};
  EXPECT_EQ(parent.value(), want_parent);

  EXPECT_EQ(disk_.list_directory("data/missing").error(), base::ErrorCode::kNotFound);
  EXPECT_EQ(disk_.list_directory("data/stray.log").error(), base::ErrorCode::kIoError);
}

TEST_F(SimDiskTest, OperationsOnClosedOrInvalidFileIdFail) {
  auto file = disk_.open("g.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  disk_.close(file.value());

  EXPECT_EQ(disk_.pwrite(file.value(), Slice::from_string("x"), 0).error(),
            base::ErrorCode::kNotFound);
  std::vector<base::u8> out(4);
  EXPECT_EQ(disk_.pread(file.value(), MutSlice(out.data(), out.size()), 0).error(),
            base::ErrorCode::kNotFound);
  EXPECT_EQ(disk_.fsync(file.value()).error(), base::ErrorCode::kNotFound);
  EXPECT_EQ(disk_.size(file.value()).error(), base::ErrorCode::kNotFound);
  EXPECT_EQ(disk_.truncate(file.value(), 0).error(), base::ErrorCode::kNotFound);
  EXPECT_EQ(disk_.size(io::kInvalidFile).error(), base::ErrorCode::kNotFound);
}

// ---------------------------------------------------------------------------
// Durability. This is the part RealDisk cannot be asked about.
// ---------------------------------------------------------------------------

// The pair the class exists for (§13): the same write is durable or not depending on
// one fsync, and the crash model can tell the difference.
TEST_F(SimDiskTest, CrashAfterFsyncLosesNothing) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng, DiskFaultConfig{.torn_write_probability = 0.5,
                                    .keep_unflushed_probability = 0.0,
                                    .io_error_probability = 0.0});
  auto file = disk.open("acked.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("durable"), 0).ok());
  ASSERT_TRUE(disk.fsync(file.value()).ok());
  EXPECT_EQ(disk.unflushed_bytes(), 0u);

  disk.crash();
  disk.power_on();  // the machine reboots before anyone can read the platter

  EXPECT_EQ(disk.bytes_lost_on_crash(), 0u);
  const std::vector<base::u8> after = read_path(disk, "acked.log");
  EXPECT_EQ(Slice(after.data(), after.size()), Slice::from_string("durable"));
}

TEST_F(SimDiskTest, CrashWithoutFsyncLosesTheUnflushedWrite) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng, DiskFaultConfig{.torn_write_probability = 0.0,
                                    .keep_unflushed_probability = 0.0,
                                    .io_error_probability = 0.0});
  auto file = disk.open("acked.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("durable"), 0).ok());
  ASSERT_TRUE(disk.fsync(file.value()).ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("extra"), 7).ok());
  EXPECT_EQ(disk.unflushed_bytes(), 5u);

  disk.crash();
  disk.power_on();  // the machine reboots before anyone can read the platter

  EXPECT_EQ(disk.crashes(), 1u);
  EXPECT_EQ(disk.bytes_lost_on_crash(), 5u);
  EXPECT_EQ(disk.unflushed_bytes(), 0u);
  const std::vector<base::u8> after = read_path(disk, "acked.log");
  EXPECT_EQ(Slice(after.data(), after.size()), Slice::from_string("durable"));
}

// A page cache does not lose a write from the middle and keep the ones after it. Five
// adjacent 4-byte writes make that checkable: whatever survives must equal a prefix of
// the full image, byte for byte, with no hole and no resurrected tail.
TEST_F(SimDiskTest, CrashNeverResurrectsAWriteIssuedAfterALostOne) {
  std::vector<base::u8> full;
  for (int i = 0; i < 5; ++i) full.insert(full.end(), std::size_t{4}, static_cast<base::u8>(i + 1));

  bool saw_partial = false;
  bool saw_survivor = false;
  for (base::u64 seed = kSeed; seed < kSeed + 64; ++seed) {
    SCOPED_TRACE(seed_note(seed));
    io::SeededRandom rng(seed);
    SimDisk disk(rng, DiskFaultConfig{.torn_write_probability = 0.2,
                                      .keep_unflushed_probability = 0.5,
                                      .io_error_probability = 0.0});
    auto file = disk.open("p.log", OpenMode::kCreate);
    ASSERT_TRUE(file.ok());
    for (base::u64 i = 0; i < 5; ++i) {
      ASSERT_NO_FATAL_FAILURE(
          pwrite_fill(disk, file.value(), static_cast<base::u8>(i + 1), 4, i * 4));
    }
    disk.crash();
    disk.power_on();  // the machine reboots before anyone can read the platter

    const std::vector<base::u8> after = read_path(disk, "p.log");
    ASSERT_LE(after.size(), full.size());
    EXPECT_EQ(after, std::vector<base::u8>(
                         full.begin(), full.begin() + static_cast<std::ptrdiff_t>(after.size())));
    saw_partial = saw_partial || after.size() < full.size();
    saw_survivor = saw_survivor || !after.empty();
  }
  EXPECT_TRUE(saw_partial);   // otherwise the fault never fired and the test proves nothing
  EXPECT_TRUE(saw_survivor);
}

TEST_F(SimDiskTest, TornWriteLeavesAStrictNonEmptyPrefix) {
  constexpr std::size_t kBytes = 64;
  std::set<std::size_t> lengths;
  for (base::u64 seed = kSeed; seed < kSeed + 32; ++seed) {
    SCOPED_TRACE(seed_note(seed));
    io::SeededRandom rng(seed);
    SimDisk disk(rng, DiskFaultConfig{.torn_write_probability = 1.0,
                                      .keep_unflushed_probability = 0.0,
                                      .io_error_probability = 0.0});
    auto file = disk.open("torn.log", OpenMode::kCreate);
    ASSERT_TRUE(file.ok());
    ASSERT_NO_FATAL_FAILURE(pwrite_fill(disk, file.value(), 0xAB, kBytes, 0));
    disk.crash();
    disk.power_on();  // the machine reboots before anyone can read the platter

    const std::vector<base::u8> after = read_path(disk, "torn.log");
    EXPECT_GE(after.size(), 1u);            // a tear applies a non-empty prefix
    EXPECT_LT(after.size(), kBytes);        // ...and a proper one
    for (base::u8 byte : after) EXPECT_EQ(byte, 0xAB);  // no gap, no zero-fill
    EXPECT_EQ(disk.bytes_lost_on_crash(), kBytes - after.size());
    lengths.insert(after.size());
  }
  EXPECT_GT(lengths.size(), 1u);  // the prefix length is drawn, not fixed
}

TEST_F(SimDiskTest, CrashInvalidatesEveryOpenFileId) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng, DiskFaultConfig{});
  auto file = disk.open("h.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("persisted"), 0).ok());
  ASSERT_TRUE(disk.fsync(file.value()).ok());

  disk.crash();
  disk.power_on();  // the machine reboots before anyone can read the platter

  std::vector<base::u8> out(9);
  EXPECT_EQ(disk.pread(file.value(), MutSlice(out.data(), out.size()), 0).error(),
            base::ErrorCode::kNotFound);
  EXPECT_EQ(disk.pwrite(file.value(), Slice::from_string("x"), 0).error(),
            base::ErrorCode::kNotFound);
  EXPECT_EQ(disk.fsync(file.value()).error(), base::ErrorCode::kNotFound);
  EXPECT_EQ(disk.size(file.value()).error(), base::ErrorCode::kNotFound);

  const std::vector<base::u8> after = read_path(disk, "h.log");  // reopen and it is all there
  EXPECT_EQ(Slice(after.data(), after.size()), Slice::from_string("persisted"));
}

// ---------------------------------------------------------------------------
// The rest of the fault menu (§14.1).
// ---------------------------------------------------------------------------

TEST_F(SimDiskTest, CorruptRandomByteFlipsExactlyOneBit) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng, DiskFaultConfig{});
  auto file = disk.open("rot.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_NO_FATAL_FAILURE(pwrite_fill(disk, file.value(), 0x5A, 128, 0));
  ASSERT_TRUE(disk.fsync(file.value()).ok());
  const std::vector<base::u8> before = read_path(disk, "rot.log");

  ASSERT_TRUE(disk.corrupt_random_byte());

  const std::vector<base::u8> after = read_path(disk, "rot.log");
  ASSERT_EQ(after.size(), before.size());
  std::size_t changed = 0;
  for (std::size_t i = 0; i < after.size(); ++i) {
    if (after[i] == before[i]) continue;
    ++changed;
    EXPECT_EQ(std::popcount(static_cast<unsigned>(after[i] ^ before[i])), 1);
  }
  EXPECT_EQ(changed, 1u);
}

// Silent rot happens on the platter. Bytes still sitting in the page cache are a
// different fault — and one a crash would erase anyway.
TEST_F(SimDiskTest, CorruptRandomByteTouchesOnlyDurableContent) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng, DiskFaultConfig{});
  EXPECT_FALSE(disk.corrupt_random_byte());  // nothing on the disk at all

  auto file = disk.open("mixed.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_NO_FATAL_FAILURE(pwrite_fill(disk, file.value(), 0xAA, 64, 0));
  EXPECT_FALSE(disk.corrupt_random_byte());  // written, but nothing is durable yet

  ASSERT_TRUE(disk.fsync(file.value()).ok());
  ASSERT_NO_FATAL_FAILURE(pwrite_fill(disk, file.value(), 0xBB, 64, 64));
  ASSERT_TRUE(disk.corrupt_random_byte());

  const std::vector<base::u8> after = read_path(disk, "mixed.log");
  ASSERT_EQ(after.size(), 128u);
  std::size_t changed = 0;
  for (std::size_t i = 0; i < 64; ++i) {
    if (after[i] != 0xAA) ++changed;
  }
  EXPECT_EQ(changed, 1u);
  for (std::size_t i = 64; i < 128; ++i) EXPECT_EQ(after[i], 0xBB);
}

TEST_F(SimDiskTest, IoErrorProbabilityFailsOperationsRatherThanIgnoringThem) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng, DiskFaultConfig{.torn_write_probability = 0.0,
                                    .keep_unflushed_probability = 0.0,
                                    .io_error_probability = 1.0});
  auto file = disk.open("bad.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());

  auto written = disk.pwrite(file.value(), Slice::from_string("abc"), 0);
  ASSERT_FALSE(written.ok());
  EXPECT_EQ(written.error(), base::ErrorCode::kIoError);

  // The failure is total: nothing reached the page cache, so there is nothing pending
  // and nothing to read back.
  auto bytes = disk.size(file.value());
  ASSERT_TRUE(bytes.ok());
  EXPECT_EQ(bytes.value(), 0u);
  EXPECT_EQ(disk.unflushed_bytes(), 0u);

  std::vector<base::u8> out(4);
  EXPECT_EQ(disk.pread(file.value(), MutSlice(out.data(), out.size()), 0).error(),
            base::ErrorCode::kIoError);
  EXPECT_EQ(disk.fsync(file.value()).error(), base::ErrorCode::kIoError);
}

// ---------------------------------------------------------------------------
// Determinism (ER-2). The trace hash in week 3 is only as good as this.
// ---------------------------------------------------------------------------

std::vector<base::u8> crash_outcome(base::u64 seed) {
  io::SeededRandom rng(seed);
  SimDisk disk(rng, DiskFaultConfig{.torn_write_probability = 0.25,
                                    .keep_unflushed_probability = 0.5,
                                    .io_error_probability = 0.0});
  if (!disk.make_directories("data/topic-0")) return {};
  auto file = disk.open("data/topic-0/seg.log", OpenMode::kCreate);
  if (!file) return {};

  const std::vector<base::u8> base_bytes(32, 0x11);
  (void)disk.pwrite(file.value(), Slice(base_bytes.data(), base_bytes.size()), 0);
  (void)disk.fsync(file.value());
  for (base::u64 i = 0; i < 8; ++i) {
    const std::vector<base::u8> chunk(9, static_cast<base::u8>(0x20 + i));
    (void)disk.pwrite(file.value(), Slice(chunk.data(), chunk.size()), 32 + i * 9);
  }
  disk.crash();
  disk.power_on();  // the machine reboots before anyone can read the platter
  return read_path(disk, "data/topic-0/seg.log");
}

TEST_F(SimDiskTest, SameSeedGivesTheSameCrashByteForByte) {
  EXPECT_EQ(crash_outcome(kSeed), crash_outcome(kSeed));
  EXPECT_EQ(crash_outcome(kSeed + 1), crash_outcome(kSeed + 1));
}

TEST_F(SimDiskTest, DifferentSeedsGiveDifferentCrashes) {
  std::set<std::vector<base::u8>> outcomes;
  for (base::u64 seed = kSeed; seed < kSeed + 24; ++seed) {
    outcomes.insert(crash_outcome(seed));
  }
  EXPECT_GT(outcomes.size(), 1u);
}

// The power-off half of crash(). Without it the dying process gets a last word it would
// never get in reality: C++ still runs destructors on the way down, and
// storage::Log::~Log() writes the sparse index. A crash that politely flushes an index
// is not a crash, and recovery would be tested against a tidier disk than one a real
// power cut leaves behind.
TEST_F(SimDiskTest, ACrashedDiskAcceptsNothingUntilItIsPoweredBackOn) {
  io::SeededRandom rng(kSeed);
  SimDisk disk(rng);
  ASSERT_TRUE(disk.make_directories("/data").ok());

  auto file = disk.open("/data/a.log", OpenMode::kCreate);
  ASSERT_TRUE(file.ok());
  ASSERT_TRUE(disk.pwrite(file.value(), Slice::from_string("durable"), 0).ok());
  ASSERT_TRUE(disk.fsync(file.value()).ok());

  disk.crash();
  EXPECT_FALSE(disk.powered()) << "seed=" << kSeed;

  // Every door is shut, including the ones a destructor would reach for.
  EXPECT_EQ(disk.open("/data/b.log", OpenMode::kCreate).error(), base::ErrorCode::kIoError);
  EXPECT_EQ(disk.make_directories("/data/sub").error(), base::ErrorCode::kIoError);
  EXPECT_EQ(disk.remove("/data/a.log").error(), base::ErrorCode::kIoError);
  EXPECT_EQ(disk.list_directory("/data").error(), base::ErrorCode::kIoError);

  disk.power_on();
  auto reopened = disk.open("/data/a.log", OpenMode::kRead);
  ASSERT_TRUE(reopened.ok()) << "seed=" << kSeed;
  EXPECT_EQ(read_path(disk, "/data/a.log").size(), 7u);
}

}  // namespace
