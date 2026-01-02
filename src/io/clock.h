#pragma once

#include "base/types.h"

namespace io {

// One of the four sources of nondeterminism behind the io/ seam.
//
// Two clocks, deliberately separate:
//   monotonic_now() drives every timeout, timer, and election deadline. It never
//     goes backwards and has no relation to wall time.
//   wall_now_ms() is for record timestamps only — metadata, never ordering.
//     Correctness must not depend on it (§10: clock skew / jumps).
class Clock {
 public:
  virtual ~Clock() = default;

  virtual base::Nanos monotonic_now() = 0;
  virtual base::i64 wall_now_ms() = 0;
};

}  // namespace io
