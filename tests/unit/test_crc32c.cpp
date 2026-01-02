#include "base/crc32c.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "io/seeded_random.h"

namespace {

using base::Slice;

// RFC 3720 / iSCSI check vectors. If the hardware path is wrong these fail loudly
// rather than corrupting every batch header written for the rest of the project.
TEST(Crc32c, KnownVectors) {
  EXPECT_EQ(base::crc32c(Slice::from_string("")), 0x00000000u);
  EXPECT_EQ(base::crc32c(Slice::from_string("123456789")), 0xE3069283u);

  const std::vector<base::u8> zeros(32, 0x00);
  EXPECT_EQ(base::crc32c(Slice(zeros.data(), zeros.size())), 0x8A9136AAu);

  const std::vector<base::u8> ones(32, 0xFF);
  EXPECT_EQ(base::crc32c(Slice(ones.data(), ones.size())), 0x62A8AB43u);
}

TEST(Crc32c, SoftwareMatchesKnownVectors) {
  EXPECT_EQ(base::crc32c_software(0, Slice::from_string("123456789")), 0xE3069283u);
}

// The hardware path is only worth having if it is indistinguishable from the
// reference. Lengths and alignments are varied because the fast path splits on both.
TEST(Crc32c, HardwareAgreesWithSoftware) {
  io::SeededRandom rng(0xC0FFEE);
  std::vector<base::u8> data(4096);
  for (base::u8& b : data) b = static_cast<base::u8>(rng.next_below(256));

  for (std::size_t len = 0; len <= 300; ++len) {
    for (std::size_t offset = 0; offset < 9; ++offset) {
      const Slice s(data.data() + offset, len);
      EXPECT_EQ(base::crc32c(0, s), base::crc32c_software(0, s))
          << "len=" << len << " offset=" << offset;
    }
  }
}

// Chunked accumulation must equal a single pass, or incremental batch checksumming
// over a scatter list silently produces a different answer than verification does.
TEST(Crc32c, IncrementalMatchesOneShot) {
  const std::string text = "the quick brown fox jumps over the lazy dog";
  const Slice all = Slice::from_string(text);

  for (std::size_t split = 0; split <= text.size(); ++split) {
    const base::u32 a = base::crc32c(0, all.subslice(0, split));
    const base::u32 b = base::crc32c(a, all.subslice(split));
    EXPECT_EQ(b, base::crc32c(all)) << "split=" << split;
  }
}

TEST(Crc32c, DetectsSingleBitFlip) {
  std::vector<base::u8> data(128, 0x5A);
  const base::u32 before = base::crc32c(Slice(data.data(), data.size()));
  data[64] ^= 0x01;
  EXPECT_NE(base::crc32c(Slice(data.data(), data.size())), before);
}

}  // namespace
