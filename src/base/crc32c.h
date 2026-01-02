#pragma once

#include "base/slice.h"
#include "base/types.h"

namespace base {

// CRC-32C (Castagnoli, polynomial 0x1EDC6F41 / reflected 0x82F63B78).
//
// Not CRC-32 (zlib). The distinction matters: CRC32C has hardware support on both
// x86-64 (SSE4.2) and ARMv8, which is why every modern log format uses it, and it is
// what Kafka's batch header specifies.
//
// crc32c(Slice{"123456789"}) == 0xE3069283 — the standard check value.
u32 crc32c(u32 crc, Slice data);

inline u32 crc32c(Slice data) { return crc32c(0, data); }

// True if this build compiled in the hardware path. Exposed so a test can assert
// the software and hardware implementations agree rather than trusting one.
bool crc32c_has_hardware_support();

// Always the portable table-driven implementation, regardless of build flags.
u32 crc32c_software(u32 crc, Slice data);

}  // namespace base
