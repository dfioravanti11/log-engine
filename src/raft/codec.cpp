#include "raft/codec.h"

#include <cstring>

#include "base/endian.h"

namespace raft {

void encode_message(base::MutSlice out, const Message& message, base::Slice entry) {
  const bool attached = message.carries_entry() && !entry.empty();
  const std::size_t entry_size = attached ? entry.size() : 0;
  if (out.size() != message_bytes(entry_size)) return;

  base::u8* p = out.data();
  for (std::size_t i = 0; i < kMessageHeaderBytes; ++i) p[i] = 0;

  p[0] = static_cast<base::u8>(message.type);
  p[1] = message.granted ? 1u : 0u;
  base::store_u16_le(p + 2, 0);
  base::store_u32_le(p + 4, message.from);
  base::store_u32_le(p + 8, message.to);
  base::store_u32_le(p + 12, static_cast<base::u32>(entry_size));
  base::store_u64_le(p + 16, message.term);
  base::store_u64_le(p + 24, message.log_index);
  base::store_u64_le(p + 32, message.log_term);
  base::store_u64_le(p + 40, message.commit_index);
  base::store_u64_le(p + 48, attached ? message.entry_index : kNoIndex);
  base::store_u64_le(p + 56, attached ? message.entry_term : kNoTerm);
  base::store_u64_le(p + 64, attached ? message.entry_end : 0);

  if (entry_size != 0) std::memcpy(p + kMessageHeaderBytes, entry.data(), entry_size);
}

base::Result<Message> decode_message(base::Slice payload, base::Slice* entry) {
  if (entry != nullptr) *entry = base::Slice();
  if (payload.size() < kMessageHeaderBytes) {
    return base::fail(base::ErrorCode::kInvalidRequest);
  }

  const base::u8* p = payload.data();
  const base::u8 type = p[0];
  if (type < static_cast<base::u8>(MessageType::kRequestVote) ||
      type > static_cast<base::u8>(MessageType::kAppendEntriesResponse)) {
    return base::fail(base::ErrorCode::kInvalidRequest);
  }

  const base::u32 entry_size = base::load_u32_le(p + 12);
  // The declared length is checked against what actually arrived before it is used to
  // index anything. This is the field a fuzzer reaches for first (§16.3).
  if (payload.size() != message_bytes(entry_size)) {
    return base::fail(base::ErrorCode::kInvalidRequest);
  }

  Message message;
  message.type = static_cast<MessageType>(type);
  // Any non-zero byte means true. Insisting on exactly 1 would reject a peer that a
  // future version encodes differently, and there is nothing to gain by being strict
  // about the bit pattern of a bool.
  message.granted = p[1] != 0;
  message.from = base::load_u32_le(p + 4);
  message.to = base::load_u32_le(p + 8);
  message.term = base::load_u64_le(p + 16);
  message.log_index = base::load_u64_le(p + 24);
  message.log_term = base::load_u64_le(p + 32);
  message.commit_index = base::load_u64_le(p + 40);
  message.entry_index = base::load_u64_le(p + 48);
  message.entry_term = base::load_u64_le(p + 56);
  message.entry_end = base::load_u64_le(p + 64);

  // "Names an entry" and "carries one" must agree, or the receiver would decide to append
  // and then have nothing to write.
  if (message.carries_entry() != (entry_size != 0)) {
    return base::fail(base::ErrorCode::kInvalidRequest);
  }

  if (entry_size != 0 && entry != nullptr) {
    *entry = payload.subslice(kMessageHeaderBytes, entry_size);
  }
  return message;
}

}  // namespace raft
