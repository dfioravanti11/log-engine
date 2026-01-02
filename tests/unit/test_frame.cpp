#include "wire/frame.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using base::Buffer;
using base::Slice;
using wire::FrameDecoder;
using wire::FrameHeader;

FrameHeader make_header(base::u32 correlation_id) {
  return FrameHeader{wire::ApiKey::kProduce, wire::kApiVersion0, correlation_id};
}

TEST(Frame, RoundTrip) {
  Buffer buf;
  wire::encode_frame(buf, make_header(42), Slice::from_string("payload"));

  FrameDecoder decoder;
  FrameHeader header;
  Slice payload;

  auto got = decoder.next(buf, &header, &payload);
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got.value());

  EXPECT_EQ(header.api_key, wire::ApiKey::kProduce);
  EXPECT_EQ(header.api_version, wire::kApiVersion0);
  EXPECT_EQ(header.correlation_id, 42u);
  EXPECT_EQ(payload, Slice::from_string("payload"));

  decoder.consume_frame(buf);
  EXPECT_TRUE(buf.empty());
}

TEST(Frame, EmptyPayload) {
  Buffer buf;
  wire::encode_frame(buf, make_header(1), Slice());
  EXPECT_EQ(buf.size(), wire::kFramePrefixBytes);

  FrameDecoder decoder;
  FrameHeader header;
  Slice payload;
  auto got = decoder.next(buf, &header, &payload);
  ASSERT_TRUE(got.ok());
  EXPECT_TRUE(got.value());
  EXPECT_TRUE(payload.empty());
}

// Pipelining depends on this: several requests in flight, responses free to arrive
// out of order, all distinguished by correlation_id.
TEST(Frame, MultipleFramesInOneBuffer) {
  Buffer buf;
  for (base::u32 i = 0; i < 5; ++i) {
    wire::encode_frame(buf, make_header(i), Slice::from_string(std::string(i, 'x')));
  }

  FrameDecoder decoder;
  for (base::u32 i = 0; i < 5; ++i) {
    FrameHeader header;
    Slice payload;
    auto got = decoder.next(buf, &header, &payload);
    ASSERT_TRUE(got.ok());
    ASSERT_TRUE(got.value()) << "frame " << i;
    EXPECT_EQ(header.correlation_id, i);
    EXPECT_EQ(payload.size(), i);
    decoder.consume_frame(buf);
  }
  EXPECT_TRUE(buf.empty());
}

// TCP delivers a byte stream, not messages. Feeding one byte at a time is the
// cheapest way to prove the decoder never assumes a frame arrives whole.
TEST(Frame, ByteAtATimeDelivery) {
  Buffer wire_bytes;
  wire::encode_frame(wire_bytes, make_header(7), Slice::from_string("hello"));

  const std::vector<base::u8> stream(wire_bytes.data(), wire_bytes.data() + wire_bytes.size());

  Buffer buf;
  FrameDecoder decoder;
  FrameHeader header;
  Slice payload;

  for (std::size_t i = 0; i + 1 < stream.size(); ++i) {
    buf.append(Slice(&stream[i], 1));
    auto got = decoder.next(buf, &header, &payload);
    ASSERT_TRUE(got.ok());
    EXPECT_FALSE(got.value()) << "decoded early at byte " << i;
  }

  buf.append(Slice(&stream[stream.size() - 1], 1));
  auto got = decoder.next(buf, &header, &payload);
  ASSERT_TRUE(got.ok());
  ASSERT_TRUE(got.value());
  EXPECT_EQ(header.correlation_id, 7u);
  EXPECT_EQ(payload, Slice::from_string("hello"));
}

// The first thing a fuzzer reaches for: a four-byte attacker-controlled length.
// Rejection must happen before anything is allocated.
TEST(Frame, RejectsOversizedLengthBeforeAllocating) {
  Buffer buf;
  buf.append_u32_le(0xFFFFFFFFu);
  buf.append_u16_le(0);
  buf.append_u16_le(0);
  buf.append_u32_le(0);

  FrameDecoder decoder(1024);
  FrameHeader header;
  Slice payload;
  auto got = decoder.next(buf, &header, &payload);
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error(), base::ErrorCode::kMessageTooLarge);
}

TEST(Frame, RejectsLengthShorterThanHeader) {
  Buffer buf;
  buf.append_u32_le(3);  // less than kHeaderBytes
  buf.append_u16_le(0);
  buf.append_u16_le(0);

  FrameDecoder decoder;
  FrameHeader header;
  Slice payload;
  auto got = decoder.next(buf, &header, &payload);
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error(), base::ErrorCode::kInvalidRequest);
}

TEST(Frame, MaxSizedFrameIsAccepted) {
  constexpr base::u32 kMax = 4096;
  const std::string payload_text(kMax - wire::kHeaderBytes, 'z');

  Buffer buf;
  wire::encode_frame(buf, make_header(9), Slice::from_string(payload_text));

  FrameDecoder decoder(kMax);
  FrameHeader header;
  Slice payload;
  auto got = decoder.next(buf, &header, &payload);
  ASSERT_TRUE(got.ok());
  EXPECT_TRUE(got.value());
  EXPECT_EQ(payload.size(), payload_text.size());
}

TEST(Frame, BytesNeededGuidesTheReader) {
  Buffer buf;
  FrameDecoder decoder;
  EXPECT_EQ(decoder.bytes_needed(buf), wire::kLengthFieldBytes);

  Buffer full;
  wire::encode_frame(full, make_header(1), Slice::from_string("abcdefgh"));
  buf.append(Slice(full.data(), 6));
  EXPECT_EQ(decoder.bytes_needed(buf), full.size() - 6);

  buf.append(Slice(full.data() + 6, full.size() - 6));
  EXPECT_EQ(decoder.bytes_needed(buf), 0u);
}

TEST(Frame, PreservesAllApiKeys) {
  const wire::ApiKey keys[] = {
      wire::ApiKey::kProduce,     wire::ApiKey::kFetch,
      wire::ApiKey::kMetadata,    wire::ApiKey::kListOffsets,
      wire::ApiKey::kOffsetCommit, wire::ApiKey::kRequestVote,
      wire::ApiKey::kAppendEntries, wire::ApiKey::kInstallSnapshot,
      wire::ApiKey::kEcho};

  for (wire::ApiKey key : keys) {
    Buffer buf;
    wire::encode_frame(buf, FrameHeader{key, 3, 11}, Slice::from_string("x"));

    FrameDecoder decoder;
    FrameHeader header;
    Slice payload;
    auto got = decoder.next(buf, &header, &payload);
    ASSERT_TRUE(got.ok());
    ASSERT_TRUE(got.value());
    EXPECT_EQ(header.api_key, key) << wire::to_string(key);
    EXPECT_EQ(header.api_version, 3u);
  }
}

}  // namespace
