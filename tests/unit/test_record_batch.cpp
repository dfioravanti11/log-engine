#include "storage/record_batch.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "base/endian.h"

namespace {

using base::Slice;
using storage::BatchBuilder;
using storage::BatchHeader;
using storage::BatchMeta;
using storage::RecordIterator;

// Batches live in a std::string so tests can corrupt them byte by byte; the production
// path never copies them out of the builder.
base::u8* mutable_bytes(std::string& s) { return reinterpret_cast<base::u8*>(s.data()); }

// Field positions the tests poke at. Named rather than inlined because the header
// layout has already moved once (see the CRC-coverage note in record_batch.h), and the
// first version of this file scattered the literal 16 and 17 through five tests.
constexpr std::size_t kBatchLengthOffset = 8;
constexpr std::size_t kMagicOffset = 20;
constexpr std::size_t kAttributesOffset = 21;
constexpr std::size_t kLastOffsetDeltaOffset = 22;
constexpr std::size_t kRecordCountOffset = 48;

std::string build(base::u64 base_offset, const BatchMeta& meta,
                  const std::vector<std::string>& records) {
  BatchBuilder builder;
  for (const std::string& r : records) {
    EXPECT_TRUE(builder.add_record(Slice::from_string(r)).ok());
  }
  const Slice framed = builder.finish(base_offset, meta);
  return std::string(reinterpret_cast<const char*>(framed.data()), framed.size());
}

std::string build_simple(base::u64 base_offset, const std::vector<std::string>& records) {
  return build(base_offset, BatchMeta{}, records);
}

// What a corrupter with a checksum implementation looks like: the bytes lie, but the
// CRC agrees with them. Only the structural checks catch that.
void restamp_crc(std::string& batch) {
  const base::u32 crc = storage::compute_crc(Slice::from_string(batch), batch.size());
  base::store_u32_le(mutable_bytes(batch) + storage::kCrcFieldOffset, crc);
}

BatchHeader header_of(const std::string& batch) {
  auto decoded = storage::decode_header(Slice::from_string(batch));
  EXPECT_TRUE(decoded.ok());
  return decoded.value();
}

TEST(RecordBatch, HeaderSurvivesRoundTrip) {
  BatchMeta meta;
  meta.leader_epoch = 7;
  meta.control = true;
  meta.timestamp_ms = -1'700'000'000'123;  // negative proves the field is signed on disk
  meta.producer_id = 0xDEADBEEFCAFEBABEull;
  meta.producer_epoch = 0xBEEF;
  meta.base_sequence = 4321;

  const std::string batch = build(1ull << 40, meta, {"alpha", "beta"});
  ASSERT_TRUE(storage::validate_batch(Slice::from_string(batch)).ok());

  const BatchHeader h = header_of(batch);
  EXPECT_EQ(h.base_offset, 1ull << 40);
  EXPECT_EQ(h.batch_length, batch.size() - storage::kBatchLengthPrefixBytes);
  EXPECT_EQ(h.leader_epoch, 7u);
  EXPECT_EQ(h.magic, storage::kMagicV0);
  EXPECT_EQ(h.attributes, storage::kAttrControl);
  EXPECT_EQ(h.last_offset_delta, 1u);
  EXPECT_EQ(h.timestamp_ms, -1'700'000'000'123);
  EXPECT_EQ(h.producer_id, 0xDEADBEEFCAFEBABEull);
  EXPECT_EQ(h.producer_epoch, 0xBEEFu);
  EXPECT_EQ(h.base_sequence, 4321u);
  EXPECT_EQ(h.record_count, 2u);

  EXPECT_EQ(h.total_bytes(), batch.size());
  EXPECT_EQ(h.crc, storage::compute_crc(Slice::from_string(batch), batch.size()));
}

TEST(RecordBatch, OffsetArithmetic) {
  const std::string batch = build_simple(100, {"a", "b", "c"});
  const BatchHeader h = header_of(batch);

  EXPECT_EQ(h.record_count, 3u);
  EXPECT_EQ(h.last_offset_delta, h.record_count - 1);
  EXPECT_EQ(h.last_offset(), 102u);
  EXPECT_EQ(h.next_offset(), 103u);
  EXPECT_FALSE(h.is_control());
}

// A control record is not a fetchable record, but it is a real offset. Anything that
// treats the offset space as dense breaks here first (FR-6).
TEST(RecordBatch, ControlBatchConsumesOffsets) {
  BatchMeta meta;
  meta.control = true;
  const std::string batch = build(500, meta, {"leader-change"});

  const BatchHeader h = header_of(batch);
  EXPECT_TRUE(h.is_control());
  EXPECT_EQ(h.record_count, 1u);
  EXPECT_EQ(h.last_offset(), 500u);
  EXPECT_EQ(h.next_offset(), 501u);
  EXPECT_TRUE(storage::validate_batch(Slice::from_string(batch)).ok());
}

TEST(RecordBatch, IteratorReturnsExactlyTheRecordsAdded) {
  std::string binary;
  binary.push_back('\0');
  binary.push_back('\x7f');
  binary.push_back(static_cast<char>(0xFE));

  const std::vector<std::string> records = {"first", "", "the third one is longer", binary};
  const std::string batch = build_simple(0, records);
  ASSERT_TRUE(storage::validate_batch(Slice::from_string(batch)).ok());

  RecordIterator it(Slice::from_string(batch));
  for (const std::string& expected : records) {
    Slice value;
    ASSERT_TRUE(it.next(&value)) << "expected record " << expected;
    EXPECT_EQ(value, Slice::from_string(expected));
  }
  Slice extra;
  EXPECT_FALSE(it.next(&extra));
}

// A read buffer normally holds several batches. The iterator must stop at its own
// batch's declared length, not at the end of the view it was handed.
TEST(RecordBatch, IteratorStopsAtEndOfItsOwnBatch) {
  const std::string first = build_simple(10, {"a1", "a2"});
  const std::string second = build_simple(12, {"b1"});
  const std::string both = first + second;

  RecordIterator it(Slice::from_string(both));
  Slice value;
  ASSERT_TRUE(it.next(&value));
  EXPECT_EQ(value, Slice::from_string("a1"));
  ASSERT_TRUE(it.next(&value));
  EXPECT_EQ(value, Slice::from_string("a2"));
  EXPECT_FALSE(it.next(&value)) << "walked into the second batch";

  RecordIterator rest(Slice::from_string(both).subslice(first.size()));
  ASSERT_TRUE(rest.next(&value));
  EXPECT_EQ(value, Slice::from_string("b1"));
  EXPECT_FALSE(rest.next(&value));
}

TEST(RecordBatch, ValidateRejectsAnyBitFlipInCrcRegion) {
  const std::string original = build_simple(3, {"payload-one", "payload-two"});
  ASSERT_TRUE(storage::validate_batch(Slice::from_string(original)).ok());

  for (std::size_t i = storage::kCrcCoveredFrom; i < original.size(); ++i) {
    std::string batch = original;
    base::u8* p = mutable_bytes(batch);
    p[i] = static_cast<base::u8>(p[i] ^ 0x01u);

    const auto status = storage::validate_batch(Slice::from_string(batch));
    ASSERT_FALSE(status.ok()) << "bit flip at byte " << i << " went unnoticed";
    EXPECT_EQ(status.error(), base::ErrorCode::kCorruptRecord);
  }
}

TEST(RecordBatch, ValidateRejectsTruncation) {
  const std::string batch = build_simple(0, {"one", "two", "three"});

  for (std::size_t len = 0; len < batch.size(); ++len) {
    const auto status = storage::validate_batch(Slice::from_string(batch).subslice(0, len));
    ASSERT_FALSE(status.ok()) << "accepted a batch truncated to " << len << " bytes";
    EXPECT_EQ(status.error(), base::ErrorCode::kCorruptRecord);
  }
}

TEST(RecordBatch, ValidateRejectsBadMagic) {
  std::string batch = build_simple(0, {"x"});
  mutable_bytes(batch)[kMagicOffset] = storage::kMagicV0 + 1u;

  // The magic is checked by decode_header before the CRC is ever computed. It has to
  // be: recovery reads the header to learn how many bytes the batch claims, and it
  // cannot afford to read a payload whose length it does not yet trust.
  const auto decoded = storage::decode_header(Slice::from_string(batch));
  EXPECT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), base::ErrorCode::kCorruptRecord);
  EXPECT_EQ(storage::validate_batch(Slice::from_string(batch)).error(),
            base::ErrorCode::kCorruptRecord);
}

TEST(RecordBatch, ValidateRejectsMutatedRecordCount) {
  const std::string original = build_simple(0, {"one", "two", "three"});

  // Count disagrees with last_offset_delta.
  std::string wrong_count = original;
  base::store_u32_le(mutable_bytes(wrong_count) + kRecordCountOffset, 2u);
  restamp_crc(wrong_count);
  EXPECT_EQ(storage::validate_batch(Slice::from_string(wrong_count)).error(),
            base::ErrorCode::kCorruptRecord);

  // Count and delta agree with each other but not with the bytes: the record walk is
  // the only thing left that can tell.
  std::string consistent_lie = original;
  base::store_u32_le(mutable_bytes(consistent_lie) + kRecordCountOffset, 2u);
  base::store_u32_le(mutable_bytes(consistent_lie) + kLastOffsetDeltaOffset, 1u);
  restamp_crc(consistent_lie);
  EXPECT_EQ(storage::validate_batch(Slice::from_string(consistent_lie)).error(),
            base::ErrorCode::kCorruptRecord);
}

TEST(RecordBatch, ValidateRejectsMutatedLastOffsetDelta) {
  std::string batch = build_simple(0, {"one", "two"});
  base::store_u32_le(mutable_bytes(batch) + kLastOffsetDeltaOffset, 9u);
  restamp_crc(batch);

  EXPECT_EQ(storage::validate_batch(Slice::from_string(batch)).error(),
            base::ErrorCode::kCorruptRecord);
}

TEST(RecordBatch, ValidateRejectsAbsurdBatchLength) {
  const std::string original = build_simple(0, {"one", "two"});

  std::string huge = original;
  base::store_u32_le(mutable_bytes(huge) + kBatchLengthOffset, 0xFFFFFFFFu);
  EXPECT_EQ(storage::decode_header(Slice::from_string(huge)).error(),
            base::ErrorCode::kCorruptRecord);
  EXPECT_EQ(storage::validate_batch(Slice::from_string(huge)).error(),
            base::ErrorCode::kCorruptRecord);

  // Too small to hold its own header, which would make every later offset negative.
  std::string tiny = original;
  base::store_u32_le(mutable_bytes(tiny) + kBatchLengthOffset, 4u);
  EXPECT_EQ(storage::decode_header(Slice::from_string(tiny)).error(),
            base::ErrorCode::kCorruptRecord);
}

// Compression is reserved and never written (§3). A batch claiming it came from a
// version this build cannot decode, not from a corrupt bit — so the CRC is restamped
// to prove the attribute check stands on its own rather than riding on the checksum.
TEST(RecordBatch, ValidateRejectsCompressedAttribute) {
  std::string batch = build_simple(0, {"one"});
  base::u8* p = mutable_bytes(batch);
  p[kAttributesOffset] = static_cast<base::u8>(p[kAttributesOffset] | storage::kAttrCompressed);
  restamp_crc(batch);

  EXPECT_EQ(storage::validate_batch(Slice::from_string(batch)).error(),
            base::ErrorCode::kCorruptRecord);
}

// Attributes are now *inside* the covered region, which is the whole point of the
// reordering: a flipped control bit would otherwise hide an acked record from every
// consumer with the checksum still verifying.
TEST(RecordBatch, FlippingTheControlBitBreaksTheCrc) {
  std::string batch = build_simple(0, {"one"});
  base::u8* p = mutable_bytes(batch);
  p[kAttributesOffset] = static_cast<base::u8>(p[kAttributesOffset] | storage::kAttrControl);

  EXPECT_EQ(storage::validate_batch(Slice::from_string(batch)).error(),
            base::ErrorCode::kCorruptRecord);
}

// The reason the CRC field sits at byte 12 rather than byte 0 (§16.2): base_offset and
// batch_length are assigned by the log, not the producer, and a replica or a recovery
// pass must be able to rewrite them without paying for a re-checksum of the payload.
TEST(RecordBatch, RewritingBaseOffsetOrBatchLengthDoesNotInvalidateCrc) {
  const std::string original = build_simple(0, {"one", "two", "three"});
  const std::size_t total = original.size();
  const base::u32 crc = header_of(original).crc;

  std::string reoffset = original;
  base::store_u64_le(mutable_bytes(reoffset), 9'999'999ull);
  EXPECT_EQ(storage::compute_crc(Slice::from_string(reoffset), total), crc);
  EXPECT_TRUE(storage::validate_batch(Slice::from_string(reoffset)).ok())
      << "reassigning base_offset must not require a re-checksum";
  EXPECT_EQ(header_of(reoffset).base_offset, 9'999'999ull);

  // batch_length is outside the covered region too, so the CRC over that region still
  // matches — the length check, not the checksum, is what rejects the batch.
  std::string relength = original;
  base::store_u32_le(mutable_bytes(relength) + kBatchLengthOffset, static_cast<base::u32>(total));
  EXPECT_EQ(storage::compute_crc(Slice::from_string(relength), total), crc);
  EXPECT_EQ(storage::validate_batch(Slice::from_string(relength)).error(),
            base::ErrorCode::kCorruptRecord);
}

TEST(RecordBatch, AddRecordRejectsRecordThatWouldOverflowTheBatch) {
  const std::size_t fits =
      storage::kMaxBatchBytes - storage::kBatchHeaderBytes - storage::kRecordLengthBytes;
  const std::vector<base::u8> value(fits, 0x5A);

  BatchBuilder builder;
  ASSERT_TRUE(builder.add_record(Slice(value.data(), value.size())).ok());
  EXPECT_EQ(builder.framed_bytes(), storage::kMaxBatchBytes);

  const base::u8 one_more = 0;
  const auto status = builder.add_record(Slice(&one_more, 1));
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.error(), base::ErrorCode::kMessageTooLarge);
  EXPECT_EQ(builder.record_count(), 1u);
}

TEST(RecordBatch, ClearMakesTheBuilderReusable) {
  BatchBuilder builder;
  EXPECT_TRUE(builder.empty());
  EXPECT_EQ(builder.framed_bytes(), storage::kBatchHeaderBytes);

  ASSERT_TRUE(builder.add_record(Slice::from_string("first-batch")).ok());
  ASSERT_TRUE(builder.add_record(Slice::from_string("second-record")).ok());
  const Slice first = builder.finish(0, BatchMeta{});
  const std::string first_copy(reinterpret_cast<const char*>(first.data()), first.size());

  builder.clear();
  EXPECT_TRUE(builder.empty());
  EXPECT_EQ(builder.record_count(), 0u);
  EXPECT_EQ(builder.framed_bytes(), storage::kBatchHeaderBytes);

  ASSERT_TRUE(builder.add_record(Slice::from_string("first-batch")).ok());
  ASSERT_TRUE(builder.add_record(Slice::from_string("second-record")).ok());
  const Slice again = builder.finish(0, BatchMeta{});
  EXPECT_EQ(again, Slice::from_string(first_copy)) << "a reused builder must be a fresh one";
  EXPECT_TRUE(storage::validate_batch(again).ok());
}

}  // namespace
