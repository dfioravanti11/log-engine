#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "base/endian.h"
#include "raft/codec.h"
#include "raft/state_file.h"

// `raft.state` is 64 bytes and it is the most safety-critical file in the repo. If it
// comes back wrong — or comes back *plausible* but wrong — a node votes twice in a term
// and two leaders get elected (I6). These tests are cheap; the failure they prevent is
// the one that takes a week to find.
namespace {

using raft::HardState;
using raft::kStateFileBytes;
using raft::kStateSlotBytes;

std::vector<base::u8> fresh_file() { return std::vector<base::u8>(kStateFileBytes, 0); }

void write_slot(std::vector<base::u8>& file, const HardState& state, base::u64 sequence) {
  const std::size_t offset = raft::state_slot_offset(sequence);
  raft::encode_state_slot(base::MutSlice(file.data() + offset, kStateSlotBytes), state,
                          sequence);
}

TEST(RaftStateFile, ASlotRoundTrips) {
  std::vector<base::u8> slot(kStateSlotBytes, 0);
  const HardState state{42, 3};
  raft::encode_state_slot(base::MutSlice(slot.data(), slot.size()), state, 7);

  auto decoded = raft::decode_state_slot(base::Slice(slot.data(), slot.size()));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().hard, state);
  EXPECT_EQ(decoded.value().sequence, 7u);
}

TEST(RaftStateFile, ANodeThatNeverVotedRoundTripsAsNotHavingVoted) {
  std::vector<base::u8> slot(kStateSlotBytes, 0);
  raft::encode_state_slot(base::MutSlice(slot.data(), slot.size()), HardState{}, 0);

  auto decoded = raft::decode_state_slot(base::Slice(slot.data(), slot.size()));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded.value().hard.term, 0u);
  EXPECT_EQ(decoded.value().hard.voted_for, raft::kNoNode);
}

TEST(RaftStateFile, EverySingleBitFlipIsCaught) {
  std::vector<base::u8> slot(kStateSlotBytes, 0);
  raft::encode_state_slot(base::MutSlice(slot.data(), slot.size()), HardState{9, 1}, 4);

  for (std::size_t byte = 0; byte < kStateSlotBytes; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<base::u8> damaged = slot;
      damaged[byte] ^= static_cast<base::u8>(1u << bit);
      EXPECT_FALSE(raft::decode_state_slot(base::Slice(damaged.data(), damaged.size())))
          << "byte " << byte << " bit " << bit << " survived undetected";
    }
  }
}

TEST(RaftStateFile, SlotsAlternateSoAWriteNeverOverwritesTheLastGoodOne) {
  EXPECT_NE(raft::state_slot_offset(0), raft::state_slot_offset(1));
  EXPECT_EQ(raft::state_slot_offset(0), raft::state_slot_offset(2));
  EXPECT_EQ(raft::state_slot_offset(1), raft::state_slot_offset(3));
}

TEST(RaftStateFile, ScanPicksTheNewerSlot) {
  std::vector<base::u8> file = fresh_file();
  write_slot(file, HardState{3, 1}, 8);
  write_slot(file, HardState{4, 2}, 9);

  auto scanned = raft::scan_state_file(base::Slice(file.data(), file.size()));
  ASSERT_TRUE(scanned);
  EXPECT_EQ(scanned.value().hard.term, 4u);
  EXPECT_EQ(scanned.value().hard.voted_for, 2u);
}

// The whole reason there are two slots. A torn write damages the half being written; the
// other half is the previous state, intact, and that is what the node comes back as.
TEST(RaftStateFile, ATornWriteOverTheNewerSlotLeavesTheOlderOneUsable) {
  std::vector<base::u8> file = fresh_file();
  write_slot(file, HardState{3, 1}, 8);
  write_slot(file, HardState{4, 2}, 9);

  const std::size_t torn = raft::state_slot_offset(9);
  for (std::size_t i = torn + 4; i < torn + kStateSlotBytes; ++i) file[i] = 0xAB;

  auto scanned = raft::scan_state_file(base::Slice(file.data(), file.size()));
  ASSERT_TRUE(scanned) << "the intact slot must still be found";
  EXPECT_EQ(scanned.value().hard.term, 3u);
  EXPECT_EQ(scanned.value().hard.voted_for, 1u);
  // Losing the newest write is *safe*: the node comes back one state behind, which costs
  // it an election at worst. Coming back with no state at all is what is unsafe.
}

// The safe direction when both halves are gone. A node whose vote is unreadable must
// stay down rather than restart as a fresh term-0 voter — down is something an operator
// can fix, a double vote is not.
TEST(RaftStateFile, AFileWithNoIntactSlotIsAnErrorAndNotAFreshStart) {
  std::vector<base::u8> file = fresh_file();
  write_slot(file, HardState{3, 1}, 8);
  write_slot(file, HardState{4, 2}, 9);
  for (base::u8& byte : file) byte ^= 0xFFu;

  auto scanned = raft::scan_state_file(base::Slice(file.data(), file.size()));
  ASSERT_FALSE(scanned);
  EXPECT_EQ(scanned.error(), base::ErrorCode::kCorruptRecord);
}

TEST(RaftStateFile, AShortFileIsRejected) {
  std::vector<base::u8> file(kStateFileBytes - 1, 0);
  EXPECT_FALSE(raft::scan_state_file(base::Slice(file.data(), file.size())));
}

TEST(RaftStateFile, AllZeroesIsNotMistakenForValidState) {
  const std::vector<base::u8> file = fresh_file();
  // A freshly created file, or one whose write never reached the platter, is all zeroes.
  // The magic byte is what stops that from decoding as "term 0, voted for node 0".
  EXPECT_FALSE(raft::scan_state_file(base::Slice(file.data(), file.size())));
}

std::vector<base::u8> encode(const raft::Message& message, base::Slice entry = {}) {
  std::vector<base::u8> bytes(raft::message_bytes(entry.size()), 0);
  raft::encode_message(base::MutSlice(bytes.data(), bytes.size()), message, entry);
  return bytes;
}

TEST(RaftMessageCodec, EveryMessageTypeRoundTrips) {
  const raft::MessageType types[] = {
      raft::MessageType::kRequestVote, raft::MessageType::kRequestVoteResponse,
      raft::MessageType::kAppendEntries, raft::MessageType::kAppendEntriesResponse};

  for (raft::MessageType type : types) {
    raft::Message message;
    message.type = type;
    message.from = 2;
    message.to = 5;
    message.term = 0x1122334455667788ull;
    message.log_index = 987654321;
    message.log_term = 4242;
    message.commit_index = 5150;
    message.granted = true;

    const std::vector<base::u8> bytes = encode(message);
    base::Slice entry;
    auto decoded = raft::decode_message(base::Slice(bytes.data(), bytes.size()), &entry);
    ASSERT_TRUE(decoded);
    const raft::Message& out = decoded.value();
    EXPECT_EQ(out.type, message.type);
    EXPECT_EQ(out.from, message.from);
    EXPECT_EQ(out.to, message.to);
    EXPECT_EQ(out.term, message.term);
    EXPECT_EQ(out.log_index, message.log_index);
    EXPECT_EQ(out.log_term, message.log_term);
    EXPECT_EQ(out.commit_index, message.commit_index);
    EXPECT_EQ(out.granted, message.granted);
    EXPECT_FALSE(out.carries_entry());
    EXPECT_TRUE(entry.empty());
  }
}

// The entry is copied verbatim — header, CRC and all. That is what makes "these two nodes
// hold the same entry" a byte-for-byte question rather than a semantic one.
TEST(RaftMessageCodec, AnAttachedEntrySurvivesByteForByte) {
  const std::string batch = "not really a batch, but it does not matter here \x01\x02\xff";

  raft::Message message;
  message.type = raft::MessageType::kAppendEntries;
  message.from = 0;
  message.to = 1;
  message.term = 9;
  message.log_index = 41;
  message.log_term = 7;
  message.entry_index = 42;
  message.entry_term = 9;

  const std::vector<base::u8> bytes = encode(message, base::Slice::from_string(batch));
  EXPECT_EQ(bytes.size(), raft::kMessageHeaderBytes + batch.size());

  base::Slice entry;
  auto decoded = raft::decode_message(base::Slice(bytes.data(), bytes.size()), &entry);
  ASSERT_TRUE(decoded);
  EXPECT_TRUE(decoded.value().carries_entry());
  EXPECT_EQ(decoded.value().entry_index, 42u);
  EXPECT_EQ(decoded.value().entry_term, 9u);
  EXPECT_EQ(entry, base::Slice::from_string(batch));
}

// A declared length that does not match what arrived is the first thing a fuzzer reaches
// for (§16.3), and the reason max-frame enforcement happens before allocation.
TEST(RaftMessageCodec, AShortOrOverlongPayloadIsRejectedAndNotAsserted) {
  raft::Message message;
  message.type = raft::MessageType::kAppendEntries;
  message.entry_index = 1;
  message.entry_term = 1;
  const std::string batch = "0123456789";
  std::vector<base::u8> bytes = encode(message, base::Slice::from_string(batch));

  base::Slice entry;
  // Truncated: the header says ten entry bytes, five arrived.
  EXPECT_FALSE(raft::decode_message(base::Slice(bytes.data(), bytes.size() - 5), &entry));
  // Header only.
  EXPECT_FALSE(
      raft::decode_message(base::Slice(bytes.data(), raft::kMessageHeaderBytes), &entry));
  // Shorter than a header.
  EXPECT_FALSE(raft::decode_message(base::Slice(bytes.data(), 3), &entry));
  EXPECT_TRUE(entry.empty()) << "a rejected decode must not leave a dangling view";
}

// "Names an entry" and "carries one" have to agree, or a receiver decides to append and
// then has nothing to write.
TEST(RaftMessageCodec, AnEntryIndexWithoutBytesIsRejected) {
  raft::Message message;
  message.type = raft::MessageType::kAppendEntries;
  std::vector<base::u8> bytes = encode(message);  // no entry attached

  // Forge an entry_index into a heartbeat.
  base::store_u64_le(bytes.data() + 48, 42);
  base::Slice entry;
  auto decoded = raft::decode_message(base::Slice(bytes.data(), bytes.size()), &entry);
  EXPECT_FALSE(decoded);
  EXPECT_EQ(decoded.error(), base::ErrorCode::kInvalidRequest);
}

TEST(RaftMessageCodec, AnUnknownMessageTypeIsRejected) {
  std::vector<base::u8> bytes = encode(raft::Message{});

  for (base::u8 type : {base::u8{0}, base::u8{5}, base::u8{255}}) {
    bytes[0] = type;
    base::Slice entry;
    auto decoded = raft::decode_message(base::Slice(bytes.data(), bytes.size()), &entry);
    EXPECT_FALSE(decoded) << "type " << static_cast<int>(type);
  }
}

TEST(RaftMessageCodec, RequestsAndTheirResponsesShareAnApiKey) {
  EXPECT_EQ(raft::api_key_for(raft::MessageType::kRequestVote), wire::ApiKey::kRequestVote);
  EXPECT_EQ(raft::api_key_for(raft::MessageType::kRequestVoteResponse),
            wire::ApiKey::kRequestVote);
  EXPECT_EQ(raft::api_key_for(raft::MessageType::kAppendEntries),
            wire::ApiKey::kAppendEntries);
  EXPECT_EQ(raft::api_key_for(raft::MessageType::kAppendEntriesResponse),
            wire::ApiKey::kAppendEntries);
  EXPECT_TRUE(wire::is_broker_api(wire::ApiKey::kRequestVote));
}

}  // namespace
