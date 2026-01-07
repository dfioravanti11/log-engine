#pragma once

#include "base/buffer.h"
#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "wire/frame.h"

namespace storage {

// A batch is one Raft entry (`project_spec.md` §16.2). That identity is the whole
// point: replication cost is amortized across every record in the batch, and there is
// exactly one durable unit to CRC, ship, and recover.
//
// Header layout, little-endian, byte offsets on the left:
//
//    0  u64  base_offset           assigned by the log, NOT covered by the CRC
//    8  u32  batch_length          bytes following this field, NOT covered
//   12  u32  crc32c                covers everything AFTER this field
//   16  u32  leader_epoch          Raft term that produced the batch
//   20  u8   magic                 format version
//   21  u8   attributes            bit 0: compressed, bit 1: control batch
//   22  u32  last_offset_delta
//   26  i64  timestamp_ms
//   34  u64  producer_id
//   42  u16  producer_epoch
//   44  u32  base_sequence
//   48  u32  record_count
//   52  [records...]
//
// The CRC sits *after* base_offset and batch_length on purpose, so those two can be
// assigned or rewritten without recomputing it. Kafka makes the same call for the
// same reason.
//
// It sits *before* everything else on purpose too, and that ordering is a correction
// to this project's own first draft (see docs/retrospective.md §2). The original field
// order put attributes at byte 17, ahead of the CRC and therefore outside it — so a
// single flipped bit could set the control-batch flag on a data batch, the fetch path
// would filter it out as internal, and a record acknowledged to a producer would
// silently vanish from every consumer with the checksum still verifying. Only the
// fields that must stay rewritable belong in front of the CRC.
inline constexpr std::size_t kBatchHeaderBytes = 52;

// Everything before this is excluded from batch_length.
inline constexpr std::size_t kBatchLengthPrefixBytes = 12;
inline constexpr std::size_t kCrcFieldOffset = 12;
inline constexpr std::size_t kCrcCoveredFrom = 16;

inline constexpr base::u8 kMagicV0 = 0;

inline constexpr base::u8 kAttrCompressed = 1u << 0;  // reserved; never set in v1 (§3)
inline constexpr base::u8 kAttrControl = 1u << 1;     // §12.2 — takes a real offset, filtered from fetch

// A batch has to fit inside a wire frame with room for the request header, or it can
// be written but never replicated. Enforced here so the failure shows up at append
// time rather than as a mysterious oversized frame in week 4.
inline constexpr base::u32 kMaxBatchBytes = 8u * 1024u * 1024u;
static_assert(kMaxBatchBytes + wire::kFramePrefixBytes < wire::kDefaultMaxFrameBytes,
              "a batch must fit in a frame with header room to spare");

// Per-record framing: u32 length | value bytes.
//
// No key field. Keys exist in Kafka for log compaction, and key-based compaction is
// an explicit non-goal (§3) — adding a field to carry a feature that will never be
// built is how formats rot.
inline constexpr std::size_t kRecordLengthBytes = 4;

struct BatchHeader {
  base::u64 base_offset = 0;
  base::u32 batch_length = 0;
  base::u32 leader_epoch = 0;
  base::u8 magic = kMagicV0;
  base::u8 attributes = 0;
  base::u32 crc = 0;
  base::u32 last_offset_delta = 0;
  base::i64 timestamp_ms = 0;
  base::u64 producer_id = 0;
  base::u16 producer_epoch = 0;
  base::u32 base_sequence = 0;
  base::u32 record_count = 0;

  [[nodiscard]] bool is_control() const noexcept { return (attributes & kAttrControl) != 0; }

  // Bytes this batch occupies on disk.
  [[nodiscard]] std::size_t total_bytes() const noexcept {
    return kBatchLengthPrefixBytes + batch_length;
  }

  [[nodiscard]] base::u64 last_offset() const noexcept { return base_offset + last_offset_delta; }

  // The offset the *next* batch starts at. Never compute this as `last_offset + 1`
  // anywhere outside the log — see FR-6; offsets are monotonic, not dense.
  [[nodiscard]] base::u64 next_offset() const noexcept { return last_offset() + 1; }
};

// Everything about a batch the caller decides that is not the offset. The offset is
// the log's to assign — that is the one field a producer may not choose.
struct BatchMeta {
  base::u32 leader_epoch = 0;
  bool control = false;
  base::i64 timestamp_ms = 0;
  base::u64 producer_id = 0;
  base::u16 producer_epoch = 0;
  base::u32 base_sequence = 0;
};

// Writes the 52 header bytes. `out` must have room; the CRC field is written as given,
// so callers that build a batch should use BatchBuilder rather than this directly.
void encode_header(base::u8* out, const BatchHeader& header);

// Reads a header. Fails if there are not enough bytes, the magic is unknown, or the
// declared length is implausible. Does NOT check the CRC — validate_batch() does that,
// and recovery wants the length before it can afford to read the payload.
base::Result<BatchHeader> decode_header(base::Slice bytes);

// CRC32C over everything after the CRC field, for a batch of `total` bytes.
base::u32 compute_crc(base::Slice framed, std::size_t total);

// Full check: header sane, all bytes present, CRC matches, records frame exactly to
// the end of the batch. This is the gate every byte read off disk passes through.
base::Status validate_batch(base::Slice framed);

// Builds one framed batch in a reusable buffer.
//
// The buffer is reused across batches (clear() keeps the allocation), which is what
// makes the steady-state append path allocation-free once the buffer has grown to its
// working size — ER-4's groundwork, with the enforcing test due in week 7.
class BatchBuilder {
 public:
  BatchBuilder() { clear(); }

  void clear();

  // Fails with kMessageTooLarge rather than growing past the cap.
  base::Status add_record(base::Slice value);

  [[nodiscard]] base::u32 record_count() const noexcept { return record_count_; }
  [[nodiscard]] bool empty() const noexcept { return record_count_ == 0; }

  // Bytes the framed batch will occupy, valid before finish() so the caller can
  // decide whether to roll a segment first.
  [[nodiscard]] std::size_t framed_bytes() const noexcept { return buf_.size(); }

  // Stamps the header and CRC and returns the framed bytes. The view is valid until
  // the next mutation of this builder. An empty batch is not a thing — a batch with
  // no records still consumes an offset range, so building one is a caller bug.
  base::Slice finish(base::u64 base_offset, const BatchMeta& meta);

 private:
  base::Buffer buf_;
  base::u32 record_count_ = 0;
};

// Walks the records of a batch that has already passed validate_batch().
//
// `framed` may extend past the end of the batch — a read buffer usually holds several
// — so the iterator stops at the batch's declared length, not at the end of the view.
class RecordIterator {
 public:
  explicit RecordIterator(base::Slice framed);

  bool next(base::Slice* value);

 private:
  base::Slice framed_;
  std::size_t pos_ = kBatchHeaderBytes;
  std::size_t end_ = kBatchHeaderBytes;
};

}  // namespace storage
