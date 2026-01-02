#include "io/real/real_random.h"

#include <random>

namespace io::real {
namespace {

base::u64 os_seed() {
  std::random_device rd;
  return (static_cast<base::u64>(rd()) << 32) ^ static_cast<base::u64>(rd());
}

}  // namespace

RealRandom::RealRandom() : RealRandom(os_seed()) {}

}  // namespace io::real
