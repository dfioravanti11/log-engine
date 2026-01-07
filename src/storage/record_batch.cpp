#include "storage/record_batch.h"

#include <cassert>
#include <cstring>

#include "base/crc32c.h"
#include "base/endian.h"

namespace storage {

using base::ErrorCode;
using base::Slice;

void encode_header(base::u8* out, const BatchHeader& h) {
  base::store_u64_le(out + 0, h.base_offset);
  base::store_u32_le(out + 8, h.batch_length);
  base::store_u32_le(out + 12, h.crc);
  base::store_u32_le(out + 16, h.leader_epoch);
  out[20] = h.magic;
  out[21] = h.attributes;
  base::store_u32_le(out + 22, h.last_offset_delta);
  base::store_i64_le(out + 26, h.timestamp_ms);
  base::store_u64_le(out + 34, h.producer_id);
  base::store_u16_le(out + 42, h.producer_epoch);
  base::store_u32_le(out + 44, h.base_sequence);
  base::store_u32_le(out + 48, h.record_count);
}

base::Result<BatchHeader> decode_header(Slice bytes) {
  if (bytes.size() < kBatchHeaderBytes) return base::fail(ErrorCode::kCorruptRecord);

  const base::u8* p = bytes.data();
  BatchHeader h;
  h.base_offset = base::load_u64_le(p + 0);
  h.batch_length = base::load_u32_le(p + 8);
  h.crc = base::load_u32_le(p + 12);
  h.leader_epoch = base::load_u32_le(p + 16);
  h.magic = p[20];
  h.attributes = p[21];
  h.last_offset_delta = base::load_u32_le(p + 22);
  h.timestamp_ms = base::load_i64_le(p + 26);
  h.producer_id = base::load_u64_le(p + 34);
  h.producer_epoch = base::load_u16_le(p + 42);
  h.base_sequence = base::load_u32_le(p + 44);
  h.record_count = base::load_u32_le(p + 48);

  // These three checks run before anything trusts `batch_length` to size a read. A
  // length field read off a disk that may have lied to us is exactly as hostile as one
  // read off a socket, and gets the same treatment (§16.3).
  if (h.magic != kMagicV0) return base::fail(ErrorCode::kCorruptRecord);
  if (h.total_bytes() < kBatchHeaderBytes) return base::fail(ErrorCode::kCorruptRecord);
  if (h.total_bytes() > kMaxBatchBytes) return base::fail(ErrorCode::kCorruptRecord);

  return h;
}

base::u32 compute_crc(Slice framed, std::size_t total) {
  return base::crc32c(framed.subslice(kCrcCoveredFrom, total - kCrcCoveredFrom));
}

base::Status validate_batch(Slice framed) {
  auto header = decode_header(framed);
  if (!header) return base::fail(header.error());
  const BatchHeader& h = header.value();

  const std::size_t total = h.total_bytes();
  if (framed.size() < total) return base::fail(ErrorCode::kCorruptRecord);

  if (compute_crc(framed, total) != h.crc) return base::fail(ErrorCode::kCorruptRecord);

  // A batch with no records would still consume an offset range, which makes the
  // offset arithmetic ambiguous. Reject it rather than pick a convention.
  if (h.record_count == 0) return base::fail(ErrorCode::kCorruptRecord);
  if (h.last_offset_delta != h.record_count - 1) return base::fail(ErrorCode::kCorruptRecord);
  if ((h.attributes & kAttrCompressed) != 0) return base::fail(ErrorCode::kCorruptRecord);

  // The CRC already proves the bytes are the ones that were written. This walk proves
  // they mean what the header says they mean: a truncation that happened *before* the
  // CRC was computed is a valid CRC over invalid content, and only a structural check
  // catches it.
  std::size_t pos = kBatchHeaderBytes;
  for (base::u32 i = 0; i < h.record_count; ++i) {
    if (total - pos < kRecordLengthBytes) return base::fail(ErrorCode::kCorruptRecord);
    const base::u32 len = base::load_u32_le(framed.data() + pos);
    pos += kRecordLengthBytes;
    if (total - pos < len) return base::fail(ErrorCode::kCorruptRecord);
    pos += len;
  }
  if (pos != total) return base::fail(ErrorCode::kCorruptRecord);

  return {};
}

void BatchBuilder::clear() {
  buf_.clear();
  record_count_ = 0;
  // Reserve the header up front so records land at their final position and finish()
  // is a stamp, not a memmove.
  base::MutSlice header = buf_.append_uninitialized(kBatchHeaderBytes);
  std::memset(header.data(), 0, header.size());
}

base::Status BatchBuilder::add_record(Slice value) {
  const std::size_t added = kRecordLengthBytes + value.size();
  if (buf_.size() + added > kMaxBatchBytes) return base::fail(ErrorCode::kMessageTooLarge);
  if (value.size() > kMaxBatchBytes) return base::fail(ErrorCode::kMessageTooLarge);

  buf_.append_u32_le(static_cast<base::u32>(value.size()));
  buf_.append(value);
  ++record_count_;
  return {};
}

Slice BatchBuilder::finish(base::u64 base_offset, const BatchMeta& meta) {
  assert(record_count_ > 0 && "a batch with no records has no offsets to occupy");

  BatchHeader h;
  h.base_offset = base_offset;
  h.batch_length = static_cast<base::u32>(buf_.size() - kBatchLengthPrefixBytes);
  h.leader_epoch = meta.leader_epoch;
  h.magic = kMagicV0;
  h.attributes = meta.control ? kAttrControl : base::u8{0};
  h.crc = 0;  // stamped below, once the bytes it covers are final
  h.last_offset_delta = record_count_ - 1;
  h.timestamp_ms = meta.timestamp_ms;
  h.producer_id = meta.producer_id;
  h.producer_epoch = meta.producer_epoch;
  h.base_sequence = meta.base_sequence;
  h.record_count = record_count_;

  base::u8* p = buf_.mutable_data();
  encode_header(p, h);
  const base::u32 crc = compute_crc(buf_.slice(), buf_.size());
  base::store_u32_le(p + kCrcFieldOffset, crc);

  return buf_.slice();
}

RecordIterator::RecordIterator(Slice framed) : framed_(framed) {
  if (framed.size() < kBatchHeaderBytes) return;
  // Clamped both ways: a corrupt length must not walk past the buffer, and one that
  // is absurdly small must not put the end before the start.
  std::size_t total = kBatchLengthPrefixBytes + base::load_u32_le(framed.data() + 8);
  if (total < kBatchHeaderBytes) total = kBatchHeaderBytes;
  end_ = total < framed.size() ? total : framed.size();
}

bool RecordIterator::next(Slice* value) {
  if (end_ - pos_ < kRecordLengthBytes) return false;
  const base::u32 len = base::load_u32_le(framed_.data() + pos_);
  pos_ += kRecordLengthBytes;
  if (end_ - pos_ < len) return false;
  *value = framed_.subslice(pos_, len);
  pos_ += len;
  return true;
}

}  // namespace storage
