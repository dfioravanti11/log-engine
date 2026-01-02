#pragma once

#include "io/clock.h"

namespace io::real {

// The only clock in the project that reads the OS. Everything above the seam gets
// its time from an io::Clock reference, which in simulation is virtual time.
class RealClock final : public Clock {
 public:
  base::Nanos monotonic_now() override;
  base::i64 wall_now_ms() override;
};

}  // namespace io::real
