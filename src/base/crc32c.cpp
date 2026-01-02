#include "base/crc32c.h"

#include <array>

#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#define LOGENGINE_CRC32C_HW 1
#elif defined(__SSE4_2__)
#include <nmmintrin.h>
#define LOGENGINE_CRC32C_HW 1
#else
#define LOGENGINE_CRC32C_HW 0
#endif

namespace base {
namespace {

constexpr u32 kReflectedPoly = 0x82F63B78u;

constexpr std::array<u32, 256> make_table() {
  std::array<u32, 256> t{};
  for (u32 i = 0; i < 256; ++i) {
    u32 c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1u) ? (kReflectedPoly ^ (c >> 1)) : (c >> 1);
    }
    t[i] = c;
  }
  return t;
}

constexpr std::array<u32, 256> kTable = make_table();

}  // namespace

u32 crc32c_software(u32 crc, Slice data) {
  u32 c = crc ^ 0xFFFFFFFFu;
  const u8* p = data.data();
  for (std::size_t i = 0; i < data.size(); ++i) {
    c = kTable[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

bool crc32c_has_hardware_support() { return LOGENGINE_CRC32C_HW != 0; }

u32 crc32c(u32 crc, Slice data) {
#if LOGENGINE_CRC32C_HW
  u32 c = crc ^ 0xFFFFFFFFu;
  const u8* p = data.data();
  std::size_t n = data.size();

  // Align to 8 bytes, then consume 8 at a time. The instructions carry the same
  // reflected-CRC convention as the table loop, so the pre/post inversion is shared.
  while (n > 0 && (reinterpret_cast<std::uintptr_t>(p) & 7u) != 0) {
#if defined(__ARM_FEATURE_CRC32)
    c = __crc32cb(c, *p);
#else
    c = _mm_crc32_u8(c, *p);
#endif
    ++p;
    --n;
  }
  while (n >= 8) {
    u64 v;
    std::memcpy(&v, p, sizeof(v));
#if defined(__ARM_FEATURE_CRC32)
    c = __crc32cd(c, v);
#else
    c = static_cast<u32>(_mm_crc32_u64(c, v));
#endif
    p += 8;
    n -= 8;
  }
  while (n > 0) {
#if defined(__ARM_FEATURE_CRC32)
    c = __crc32cb(c, *p);
#else
    c = _mm_crc32_u8(c, *p);
#endif
    ++p;
    --n;
  }
  return c ^ 0xFFFFFFFFu;
#else
  return crc32c_software(crc, data);
#endif
}

}  // namespace base
