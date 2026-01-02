#pragma once

#include <cstring>

#include "base/types.h"

// Little-endian load/store. Everything this project writes — on disk and on the
// wire — is little-endian, so there is exactly one set of accessors and no
// host-order path to get wrong. Written as shifts rather than memcpy+bswap so the
// code is correct on any host; compilers fold these into a single load/store.
namespace base {

inline void store_u16_le(u8* p, u16 v) noexcept {
  p[0] = static_cast<u8>(v & 0xFFu);
  p[1] = static_cast<u8>((v >> 8) & 0xFFu);
}

inline void store_u32_le(u8* p, u32 v) noexcept {
  p[0] = static_cast<u8>(v & 0xFFu);
  p[1] = static_cast<u8>((v >> 8) & 0xFFu);
  p[2] = static_cast<u8>((v >> 16) & 0xFFu);
  p[3] = static_cast<u8>((v >> 24) & 0xFFu);
}

inline void store_u64_le(u8* p, u64 v) noexcept {
  store_u32_le(p, static_cast<u32>(v & 0xFFFFFFFFu));
  store_u32_le(p + 4, static_cast<u32>((v >> 32) & 0xFFFFFFFFu));
}

inline void store_i64_le(u8* p, i64 v) noexcept { store_u64_le(p, static_cast<u64>(v)); }

inline u16 load_u16_le(const u8* p) noexcept {
  return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8));
}

inline u32 load_u32_le(const u8* p) noexcept {
  return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
         (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}

inline u64 load_u64_le(const u8* p) noexcept {
  return static_cast<u64>(load_u32_le(p)) | (static_cast<u64>(load_u32_le(p + 4)) << 32);
}

inline i64 load_i64_le(const u8* p) noexcept { return static_cast<i64>(load_u64_le(p)); }

}  // namespace base
