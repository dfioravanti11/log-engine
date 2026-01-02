#pragma once

#include "base/types.h"

namespace io {

// The single source of randomness in the system. Nothing above the seam may call
// rand(), std::random_device, or a self-seeded engine — in simulation every random
// choice must trace back to the one seed, or the run stops being reproducible and
// the whole correctness story collapses (ER-2).
class Random {
 public:
  virtual ~Random() = default;

  virtual base::u64 next_u64() = 0;

  // Uniform in [0, bound). Undefined for bound == 0.
  virtual base::u64 next_below(base::u64 bound) = 0;

  // Uniform in [lo, hi]. Used for election-timeout jitter and fault injection.
  base::i64 next_in_range(base::i64 lo, base::i64 hi) {
    if (hi <= lo) return lo;
    const base::u64 span = static_cast<base::u64>(hi - lo) + 1;
    return lo + static_cast<base::i64>(next_below(span));
  }

  bool next_bool_with_probability(double p) {
    if (p <= 0.0) return false;
    if (p >= 1.0) return true;
    constexpr double kScale = 1.0 / 9007199254740992.0;  // 2^53
    const double u = static_cast<double>(next_u64() >> 11) * kScale;
    return u < p;
  }
};

}  // namespace io
