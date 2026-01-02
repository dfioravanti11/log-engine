#include "io/real/real_clock.h"

#include <ctime>

namespace io::real {

base::Nanos RealClock::monotonic_now() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<base::Nanos>(ts.tv_sec) * base::kNanosPerSecond +
         static_cast<base::Nanos>(ts.tv_nsec);
}

base::i64 RealClock::wall_now_ms() {
  struct timespec ts {};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<base::i64>(ts.tv_sec) * 1000 +
         static_cast<base::i64>(ts.tv_nsec) / 1'000'000;
}

}  // namespace io::real
