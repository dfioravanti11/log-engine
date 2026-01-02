#pragma once

#include "io/random.h"

namespace io {

// xoshiro256** — deterministic, fast, and small enough that its whole state can be
// printed next to a failing seed. Lives above real/ and sim/ because both use it:
// the simulator seeds it from the run seed, the real runtime seeds it from the OS.
class SeededRandom final : public Random {
 public:
  explicit SeededRandom(base::u64 seed) { reseed(seed); }

  void reseed(base::u64 seed) {
    // SplitMix64 to expand one seed word into the 256-bit state. Seeding xoshiro
    // directly from a small integer leaves the first few outputs correlated.
    for (base::u64& word : s_) {
      seed += 0x9E3779B97F4A7C15ull;
      base::u64 z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
      word = z ^ (z >> 31);
    }
  }

  base::u64 next_u64() override {
    const base::u64 result = rotl(s_[1] * 5, 7) * 9;
    const base::u64 t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Lemire's debiased bounded generation: no modulo bias, one multiply in the
  // common case. Bias would be invisible in tests and would quietly skew every
  // fault-injection distribution in the simulator.
  base::u64 next_below(base::u64 bound) override {
    if (bound <= 1) return 0;
    const base::u64 threshold = (~bound + 1) % bound;  // 2^64 mod bound
    while (true) {
      const base::u64 r = next_u64();
      if (r >= threshold) return r % bound;
    }
  }

 private:
  static constexpr base::u64 rotl(base::u64 x, int k) {
    return (x << k) | (x >> (64 - k));
  }

  base::u64 s_[4] = {};
};

}  // namespace io
