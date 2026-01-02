#include "base/buffer.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using base::Buffer;
using base::Slice;

TEST(Buffer, StartsEmpty) {
  Buffer buf;
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0u);
}

TEST(Buffer, LittleEndianRoundTrip) {
  Buffer buf;
  buf.append_u16_le(0x0201);
  buf.append_u32_le(0x08070605u);
  buf.append_u64_le(0x1817161514131211ull);
  buf.append_i64_le(-1);

  ASSERT_EQ(buf.size(), 2u + 4u + 8u + 8u);
  const base::u8* p = buf.data();

  // Byte order is asserted explicitly, not just via a round-trip: a matching
  // store/load pair would agree with itself even if both were big-endian, and the
  // on-disk format would only reveal it on a different machine.
  EXPECT_EQ(p[0], 0x01);
  EXPECT_EQ(p[1], 0x02);
  EXPECT_EQ(p[2], 0x05);
  EXPECT_EQ(p[5], 0x08);

  EXPECT_EQ(base::load_u16_le(p), 0x0201u);
  EXPECT_EQ(base::load_u32_le(p + 2), 0x08070605u);
  EXPECT_EQ(base::load_u64_le(p + 6), 0x1817161514131211ull);
  EXPECT_EQ(base::load_i64_le(p + 14), -1);
}

TEST(Buffer, ConsumeAdvancesFront) {
  Buffer buf;
  buf.append(Slice::from_string("hello world"));
  buf.consume(6);

  ASSERT_EQ(buf.size(), 5u);
  EXPECT_EQ(buf.slice(), Slice::from_string("world"));
}

TEST(Buffer, ConsumeAllResets) {
  Buffer buf;
  buf.append(Slice::from_string("abc"));
  buf.consume(3);
  EXPECT_TRUE(buf.empty());

  buf.append(Slice::from_string("def"));
  EXPECT_EQ(buf.slice(), Slice::from_string("def"));
}

// The whole point of the read_pos_/compaction scheme: many small consume+append
// cycles must not grow the allocation without bound.
TEST(Buffer, CompactsUnderStreamingUse) {
  Buffer buf;
  const std::string chunk(64, 'x');

  for (int i = 0; i < 10'000; ++i) {
    buf.append(Slice::from_string(chunk));
    buf.consume(chunk.size());
  }

  EXPECT_TRUE(buf.empty());
  EXPECT_LT(buf.capacity(), 64u * 1024u);
}

TEST(Buffer, PartialConsumeKeepsRemainderIntact) {
  Buffer buf;
  for (int i = 0; i < 100; ++i) {
    buf.append(Slice::from_string("0123456789"));
    if (i % 3 == 0) buf.consume(5);
  }
  // 100 appends of 10 bytes, minus 34 consumes of 5.
  EXPECT_EQ(buf.size(), 100u * 10u - 34u * 5u);
}

TEST(Buffer, AppendUninitializedThenShrink) {
  Buffer buf;
  buf.append(Slice::from_string("ab"));

  base::MutSlice dst = buf.append_uninitialized(8);
  ASSERT_EQ(dst.size(), 8u);
  dst.data()[0] = 'c';
  dst.data()[1] = 'd';

  buf.shrink_by(6);  // reader only filled 2 of the 8 bytes
  EXPECT_EQ(buf.slice(), Slice::from_string("abcd"));
}

}  // namespace
