#pragma once

#include <cstddef>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "raft/types.h"
#include "wire/api.h"

namespace raft {

// Broker-to-broker message payloads, carried inside a week-1 frame (§16.3):
//
//   u32 length | u16 api_key | u16 api_version | u32 correlation_id | payload
//
// A fixed 64-byte header, optionally followed by **one record batch** — the entry.
//
//   0   u8  type
//   1   u8  granted
//   2   u16 reserved
//   4   u32 from
//   8   u32 to
//   12  u32 entry_bytes     // length of the batch that follows; 0 for a heartbeat
//   16  u64 term
//   24  u64 log_index
//   32  u64 log_term
//   40  u64 commit_index
//   48  u64 entry_index
//   56  u64 entry_term
//   64  u64 entry_end       // exclusive; NOT entry_index + 1, see Message::entry_end
//   72  [batch...]
//
// The batch is copied verbatim, header and CRC included, and is *not* re-framed on the
// way through. That is what makes "these two nodes hold the same entry" a byte-for-byte
// question rather than a semantic one, and it is why a follower can validate the CRC the
// leader computed rather than trusting the network.
//
// Week 4 shipped this as a flat 48 bytes with no entry fields. The layout changed rather
// than the api_version, because there is no deployed v0 to stay compatible with —
// versioning every API from day one buys the *option* of a clean migration, and spending
// it before anything has shipped would be ceremony rather than compatibility work. The
// first real bump will be a real one.
inline constexpr std::size_t kMessageHeaderBytes = 72;

// Bytes needed to encode `message` with `entry` attached.
constexpr std::size_t message_bytes(std::size_t entry_size) noexcept {
  return kMessageHeaderBytes + entry_size;
}

// `out` must be exactly message_bytes(entry.size()). Pass an empty `entry` for a
// heartbeat; passing one for a message whose `entry_index` is kNoIndex is a caller bug
// and encodes as a heartbeat.
void encode_message(base::MutSlice out, const Message& message, base::Slice entry);

// Decodes the header and, if one is attached, points `*entry` at the batch bytes inside
// `payload` — a view, not a copy, valid only as long as `payload` is.
//
// kInvalidRequest on a short payload, an unknown message type, or a declared entry length
// that does not match what actually arrived. All three are reachable from a hostile or
// simply mismatched peer, so none of them may assert.
base::Result<Message> decode_message(base::Slice payload, base::Slice* entry);

// The api_key a message travels under. Requests and their responses share a key — the
// `type` byte in the payload is authoritative — so that a packet capture or a trace line
// groups a vote round-trip together instead of splitting it across two keys.
constexpr wire::ApiKey api_key_for(MessageType type) noexcept {
  switch (type) {
    case MessageType::kRequestVote:
    case MessageType::kRequestVoteResponse:
      return wire::ApiKey::kRequestVote;
    case MessageType::kAppendEntries:
    case MessageType::kAppendEntriesResponse:
      return wire::ApiKey::kAppendEntries;
  }
  return wire::ApiKey::kAppendEntries;
}

}  // namespace raft
