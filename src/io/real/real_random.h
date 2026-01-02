#pragma once

#include "io/seeded_random.h"

namespace io::real {

// SeededRandom with an OS-derived seed. The seed is retained and printable, because
// even in production a reproducible run beats a mysterious one.
class RealRandom final : public Random {
 public:
  RealRandom();
  explicit RealRandom(base::u64 seed) : seed_(seed), impl_(seed) {}

  [[nodiscard]] base::u64 seed() const noexcept { return seed_; }

  base::u64 next_u64() override { return impl_.next_u64(); }
  base::u64 next_below(base::u64 bound) override { return impl_.next_below(bound); }

 private:
  base::u64 seed_;
  SeededRandom impl_;
};

}  // namespace io::real
