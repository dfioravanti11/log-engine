#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "io/seeded_random.h"
#include "wire/frame.h"

// Property tests for the framing codec. Every case is driven from a fixed seed, so a
// failure is reproducible from the seed printed in the assertion rather than being a
// one-off that vanishes on rerun — the same contract the simulator will run on.
namespace {

using base::Buffer;
using base::Slice;
using wire::FrameDecoder;
using wire::FrameHeader;

constexpr base::u64 kSeed = 0x5EED'0001;

std::vector<base::u8> random_payload(io::SeededRandom& rng, std::size_t size) {
  std::vector<base::u8> payload(size);
  for (base::u8& b : payload) b = static_cast<base::u8>(rng.next_below(256));
  return payload;
}

// Anything encoded must decode back to exactly what went in — for any header, any
// payload, any length.
TEST(FrameProperty, EncodeDecodeRoundTrip) {
  io::SeededRandom rng(kSeed);

  for (int trial = 0; trial < 2000; ++trial) {
    const FrameHeader written{
        static_cast<wire::ApiKey>(rng.next_below(1024)),
        static_cast<base::u16>(rng.next_below(65536)),
        static_cast<base::u32>(rng.next_u64() & 0xFFFFFFFFu)};
    const auto payload = random_payload(rng, rng.next_below(2048));

    Buffer buf;
    wire::encode_frame(buf, written, Slice(payload.data(), payload.size()));

    FrameDecoder decoder;
    FrameHeader read;
    Slice decoded;
    auto got = decoder.next(buf, &read, &decoded);

    ASSERT_TRUE(got.ok()) << "trial " << trial;
    ASSERT_TRUE(got.value()) << "trial " << trial;
    EXPECT_EQ(read.api_key, written.api_key) << "trial " << trial;
    EXPECT_EQ(read.api_version, written.api_version) << "trial " << trial;
    EXPECT_EQ(read.correlation_id, written.correlation_id) << "trial " << trial;
    EXPECT_EQ(decoded, Slice(payload.data(), payload.size())) << "trial " << trial;

    decoder.consume_frame(buf);
    EXPECT_TRUE(buf.empty()) << "trial " << trial;
  }
}

// TCP splits wherever it likes. Chopping the stream at random boundaries is the only
// honest way to test a decoder that will spend its life reading partial frames.
TEST(FrameProperty, SurvivesArbitraryStreamChunking) {
  io::SeededRandom rng(kSeed + 1);

  for (int trial = 0; trial < 300; ++trial) {
    const int frame_count = static_cast<int>(rng.next_below(8)) + 1;

    Buffer encoded;
    std::vector<std::vector<base::u8>> payloads;
    std::vector<base::u32> correlations;

    for (int i = 0; i < frame_count; ++i) {
      auto payload = random_payload(rng, rng.next_below(500));
      const auto correlation = static_cast<base::u32>(rng.next_u64() & 0xFFFFFFFFu);
      wire::encode_frame(encoded,
                         FrameHeader{wire::ApiKey::kFetch, 0, correlation},
                         Slice(payload.data(), payload.size()));
      payloads.push_back(std::move(payload));
      correlations.push_back(correlation);
    }

    const std::vector<base::u8> stream(encoded.data(), encoded.data() + encoded.size());

    Buffer buf;
    FrameDecoder decoder;
    int decoded_count = 0;
    std::size_t position = 0;

    while (position < stream.size()) {
      const std::size_t remaining = stream.size() - position;
      const std::size_t chunk = rng.next_below(remaining) + 1;
      buf.append(Slice(stream.data() + position, chunk));
      position += chunk;

      while (true) {
        FrameHeader header;
        Slice payload;
        auto got = decoder.next(buf, &header, &payload);
        ASSERT_TRUE(got.ok()) << "trial " << trial;
        if (!got.value()) break;

        ASSERT_LT(decoded_count, frame_count) << "trial " << trial;
        const auto idx = static_cast<std::size_t>(decoded_count);
        EXPECT_EQ(header.correlation_id, correlations[idx]) << "trial " << trial;
        EXPECT_EQ(payload, Slice(payloads[idx].data(), payloads[idx].size()))
            << "trial " << trial;
        decoder.consume_frame(buf);
        ++decoded_count;
      }
    }

    EXPECT_EQ(decoded_count, frame_count) << "trial " << trial;
    EXPECT_TRUE(buf.empty()) << "trial " << trial;
  }
}

// Corrupted input must produce a decision — a frame, "need more", or a typed error —
// and must never read out of bounds. This is the shape the week-2 libFuzzer target
// takes; running it seeded here means the decoder is already hardened before the
// fuzzer ever gets to it.
TEST(FrameProperty, GarbageNeverCrashesTheDecoder) {
  io::SeededRandom rng(kSeed + 2);

  for (int trial = 0; trial < 5000; ++trial) {
    const auto bytes = random_payload(rng, rng.next_below(64));

    Buffer buf;
    buf.append(Slice(bytes.data(), bytes.size()));

    FrameDecoder decoder(4096);
    FrameHeader header;
    Slice payload;
    auto got = decoder.next(buf, &header, &payload);

    if (!got.ok()) {
      EXPECT_TRUE(got.error() == base::ErrorCode::kMessageTooLarge ||
                  got.error() == base::ErrorCode::kInvalidRequest)
          << "unexpected error " << base::to_string(got.error());
    } else if (got.value()) {
      // If it claims a frame, the payload must lie inside the bytes we supplied.
      EXPECT_LE(payload.size() + wire::kFramePrefixBytes, bytes.size());
    }
  }
}

// A length field one byte past the limit must be rejected; one byte under must be
// accepted. Off-by-one here is either a spurious disconnect or an unbounded read.
TEST(FrameProperty, MaxFrameBoundaryIsExact) {
  io::SeededRandom rng(kSeed + 3);

  for (int trial = 0; trial < 200; ++trial) {
    const auto limit = static_cast<base::u32>(rng.next_below(4096) + wire::kHeaderBytes);

    {
      Buffer buf;
      buf.append_u32_le(limit);
      buf.append_u16_le(0);
      buf.append_u16_le(0);
      buf.append_u32_le(0);
      FrameDecoder decoder(limit);
      FrameHeader header;
      Slice payload;
      auto got = decoder.next(buf, &header, &payload);
      EXPECT_TRUE(got.ok()) << "exactly at the limit must be allowed";
    }
    {
      Buffer buf;
      buf.append_u32_le(limit + 1);
      buf.append_u16_le(0);
      buf.append_u16_le(0);
      buf.append_u32_le(0);
      FrameDecoder decoder(limit);
      FrameHeader header;
      Slice payload;
      auto got = decoder.next(buf, &header, &payload);
      ASSERT_FALSE(got.ok()) << "one past the limit must be rejected";
      EXPECT_EQ(got.error(), base::ErrorCode::kMessageTooLarge);
    }
  }
}

}  // namespace
