#include "raft/state_file.h"

#include <algorithm>

#include "base/crc32c.h"
#include "base/endian.h"

namespace raft {
namespace {

constexpr std::size_t kCrcOffset = 0;
constexpr std::size_t kCrcCoveredFrom = 4;

}  // namespace

void encode_state_slot(base::MutSlice out, const HardState& state, base::u64 sequence) {
  if (out.size() != kStateSlotBytes) return;

  base::u8* p = out.data();
  // Zeroed first so the reserved fields are never uninitialized. Hashing or CRC-ing a
  // padding byte that nobody wrote is the determinism bug ER-2 exists to prevent, and it
  // would surface as a raft.state file that fails its own checksum on another machine.
  for (std::size_t i = 0; i < kStateSlotBytes; ++i) p[i] = 0;

  p[4] = kStateMagic;
  p[5] = kStateVersion;
  base::store_u16_le(p + 6, 0);
  base::store_u64_le(p + 8, sequence);
  base::store_u64_le(p + 16, state.term);
  base::store_u32_le(p + 24, state.voted_for);
  base::store_u32_le(p + 28, 0);

  const base::u32 crc =
      base::crc32c(base::Slice(p + kCrcCoveredFrom, kStateSlotBytes - kCrcCoveredFrom));
  base::store_u32_le(p + kCrcOffset, crc);
}

base::Result<PersistedState> decode_state_slot(base::Slice slot) {
  if (slot.size() != kStateSlotBytes) return base::fail(base::ErrorCode::kCorruptRecord);

  const base::u8* p = slot.data();
  if (p[4] != kStateMagic) return base::fail(base::ErrorCode::kCorruptRecord);
  if (p[5] != kStateVersion) return base::fail(base::ErrorCode::kUnsupportedVersion);

  const base::u32 stored = base::load_u32_le(p + kCrcOffset);
  const base::u32 actual =
      base::crc32c(base::Slice(p + kCrcCoveredFrom, kStateSlotBytes - kCrcCoveredFrom));
  if (stored != actual) return base::fail(base::ErrorCode::kCorruptRecord);

  PersistedState out;
  out.sequence = base::load_u64_le(p + 8);
  out.hard.term = base::load_u64_le(p + 16);
  out.hard.voted_for = base::load_u32_le(p + 24);
  return out;
}

base::Result<PersistedState> scan_state_file(base::Slice file) {
  if (file.size() < kStateSlotBytes) return base::fail(base::ErrorCode::kCorruptRecord);

  bool found = false;
  PersistedState best;

  // Only whole slots. A file can legitimately be half-sized — the very first write
  // creates 32 bytes, not 64 — and refusing to read that would leave a node that has
  // voted exactly once unable to ever start again.
  const std::size_t slots = std::min(file.size(), kStateFileBytes) / kStateSlotBytes;
  for (std::size_t offset = 0; offset < slots * kStateSlotBytes; offset += kStateSlotBytes) {
    auto slot = decode_state_slot(file.subslice(offset, kStateSlotBytes));
    if (!slot) continue;  // this half was mid-write when the power went out
    if (!found || slot.value().sequence > best.sequence) {
      best = slot.value();
      found = true;
    }
  }

  if (!found) return base::fail(base::ErrorCode::kCorruptRecord);
  return best;
}

}  // namespace raft
