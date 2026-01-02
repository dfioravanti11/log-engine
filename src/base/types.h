#pragma once

#include <cstddef>
#include <cstdint>

namespace base {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

// Monotonic duration/instant, nanoseconds.
//
// The project deliberately does NOT use <chrono> above the io/ seam (ER-1): a
// chrono clock is an OS call, and an OS call in storage/, raft/, server/, or
// client/ is exactly what makes the simulator impossible. Nanos is a plain
// integer that the simulator can hand out from virtual time.
using Nanos = i64;

inline constexpr Nanos kNanosPerMicro = 1'000;
inline constexpr Nanos kNanosPerMilli = 1'000'000;
inline constexpr Nanos kNanosPerSecond = 1'000'000'000;

// A negative timeout means "block indefinitely".
inline constexpr Nanos kNoTimeout = -1;

constexpr Nanos micros(i64 n) noexcept { return n * kNanosPerMicro; }
constexpr Nanos millis(i64 n) noexcept { return n * kNanosPerMilli; }
constexpr Nanos seconds(i64 n) noexcept { return n * kNanosPerSecond; }

constexpr double to_millis_f(Nanos n) noexcept {
  return static_cast<double>(n) / static_cast<double>(kNanosPerMilli);
}
constexpr double to_seconds_f(Nanos n) noexcept {
  return static_cast<double>(n) / static_cast<double>(kNanosPerSecond);
}

}  // namespace base
