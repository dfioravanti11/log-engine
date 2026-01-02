#include <gtest/gtest.h>

#include <vector>

#include "io/seeded_random.h"

namespace {

// The property the entire correctness story rests on: one seed, one sequence,
// forever. If this ever fails, no bug in docs/retrospective.md is reproducible and
// the simulator is just an expensive random test.
TEST(SeededRandom, SameSeedSameSequence) {
  io::SeededRandom a(0x3f2a91c4);
  io::SeededRandom b(0x3f2a91c4);

  for (int i = 0; i < 1000; ++i) {
    EXPECT_EQ(a.next_u64(), b.next_u64()) << "diverged at draw " << i;
  }
}

TEST(SeededRandom, DifferentSeedsDiverge) {
  io::SeededRandom a(1);
  io::SeededRandom b(2);

  int same = 0;
  for (int i = 0; i < 100; ++i) {
    if (a.next_u64() == b.next_u64()) ++same;
  }
  EXPECT_LT(same, 3);
}

// Adjacent seeds must not produce correlated openings — this is why the state is
// expanded through SplitMix64 instead of being set from the seed directly.
TEST(SeededRandom, AdjacentSeedsAreNotCorrelated) {
  for (base::u64 seed = 0; seed < 32; ++seed) {
    io::SeededRandom a(seed);
    io::SeededRandom b(seed + 1);
    EXPECT_NE(a.next_u64(), b.next_u64()) << "seed " << seed;
  }
}

TEST(SeededRandom, ReseedRestartsTheSequence) {
  io::SeededRandom rng(7);
  std::vector<base::u64> first;
  for (int i = 0; i < 10; ++i) first.push_back(rng.next_u64());

  rng.reseed(7);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(rng.next_u64(), first[static_cast<std::size_t>(i)]);
  }
}

TEST(SeededRandom, NextBelowStaysInRange) {
  io::SeededRandom rng(99);
  for (base::u64 bound : {1ull, 2ull, 7ull, 256ull, 1000ull}) {
    for (int i = 0; i < 500; ++i) {
      EXPECT_LT(rng.next_below(bound), bound);
    }
  }
  EXPECT_EQ(rng.next_below(0), 0u);
}

// Modulo bias would be invisible here and would quietly skew every fault-injection
// distribution in the simulator toward the low end of its range.
TEST(SeededRandom, NextBelowIsRoughlyUniform) {
  io::SeededRandom rng(0xBEEF);
  constexpr base::u64 kBuckets = 8;
  constexpr int kDraws = 80'000;

  std::vector<int> counts(kBuckets, 0);
  for (int i = 0; i < kDraws; ++i) {
    counts[static_cast<std::size_t>(rng.next_below(kBuckets))]++;
  }

  const int expected = kDraws / static_cast<int>(kBuckets);
  for (std::size_t i = 0; i < kBuckets; ++i) {
    EXPECT_NEAR(counts[i], expected, expected / 10) << "bucket " << i;
  }
}

TEST(SeededRandom, NextInRangeIsInclusive) {
  io::SeededRandom rng(5);
  bool saw_lo = false;
  bool saw_hi = false;
  for (int i = 0; i < 2000; ++i) {
    const base::i64 v = rng.next_in_range(10, 13);
    ASSERT_GE(v, 10);
    ASSERT_LE(v, 13);
    if (v == 10) saw_lo = true;
    if (v == 13) saw_hi = true;
  }
  EXPECT_TRUE(saw_lo);
  EXPECT_TRUE(saw_hi);

  EXPECT_EQ(rng.next_in_range(4, 4), 4);
  EXPECT_EQ(rng.next_in_range(9, 3), 9) << "inverted range collapses to lo";
}

TEST(SeededRandom, ProbabilityBoundsAreExact) {
  io::SeededRandom rng(11);
  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(rng.next_bool_with_probability(0.0));
    EXPECT_TRUE(rng.next_bool_with_probability(1.0));
  }

  int hits = 0;
  for (int i = 0; i < 10'000; ++i) {
    if (rng.next_bool_with_probability(0.25)) ++hits;
  }
  EXPECT_NEAR(hits, 2500, 250);
}

}  // namespace
