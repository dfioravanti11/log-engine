#pragma once

#include <cstddef>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "raft/types.h"

namespace raft {

// `raft.state` — the tiny file the whole safety argument rests on (§16.2, §13).
//
// It holds `currentTerm` and `votedFor`, and it is rewritten in place every time either
// changes, which for an election-heavy run is often. That makes it the one place in the
// system where a **torn write** is not a rare curiosity but a routine event, and where
// surviving one is not optional: a node that comes back unable to read its own vote is
// an amnesiac that can vote twice in a term and elect two leaders (I6).
//
// So the file is two 32-byte slots, written alternately, each with its own CRC and a
// sequence number. A torn write can only ever damage the slot being written, so the
// previous one is still there and still valid. Load picks the valid slot with the higher
// sequence.
//
// *Alternative considered:* write to a temp file, fsync, rename. Standard, and correct
// on a real filesystem, but it leans on rename atomicity and directory-fsync semantics —
// two things the simulated disk would then have to model faithfully or the test would be
// a test of the model rather than of the code. Two slots need nothing but pwrite and
// fsync, which are already modelled honestly.
//
// The layout mirrors the batch header's choice (§16.2): the CRC sits first and covers
// everything after it.
//
//   0   u32 crc32c      covers bytes 4..31
//   4   u8  magic       'R'
//   5   u8  version     0
//   6   u16 reserved
//   8   u64 sequence
//   16  u64 term
//   24  u32 voted_for   kNoNode when this node has not voted in `term`
//   28  u32 reserved
//
inline constexpr std::size_t kStateSlotBytes = 32;
inline constexpr std::size_t kStateFileBytes = kStateSlotBytes * 2;
inline constexpr base::u8 kStateMagic = 0x52;  // 'R'
inline constexpr base::u8 kStateVersion = 0;

struct PersistedState {
  HardState hard;
  base::u64 sequence = 0;
};

// Which half of the file a given sequence number belongs in.
constexpr std::size_t state_slot_offset(base::u64 sequence) noexcept {
  return static_cast<std::size_t>(sequence % 2) * kStateSlotBytes;
}

// `out` must be exactly kStateSlotBytes.
void encode_state_slot(base::MutSlice out, const HardState& state, base::u64 sequence);

// kCorruptRecord if the slot is the wrong size, has a bad magic, or fails its CRC.
base::Result<PersistedState> decode_state_slot(base::Slice slot);

// Picks the newest intact slot out of a whole-file image.
//
// Fails with kCorruptRecord when the file is present but *neither* slot survives. That
// is deliberate and it is the safe direction: a node whose recorded vote is unreadable
// must stay down, because coming back up as a fresh term-0 voter is exactly the amnesia
// the file exists to prevent. Down is recoverable by an operator; a double vote is not.
//
// A file that does not exist at all is a different thing — a node that has genuinely
// never voted — and the caller distinguishes those two cases, not this function.
base::Result<PersistedState> scan_state_file(base::Slice file);

}  // namespace raft
