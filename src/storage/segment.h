#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "io/disk.h"
#include "storage/record_batch.h"
#include "storage/sparse_index.h"

namespace storage {

// What the tail scan found. Carried out of open() rather than logged, because the
// caller (and the week-2 demo, and log-dump) all want to say something specific about
// it, and a recovery that only prints is a recovery you cannot test.
struct RecoveryReport {
  base::u64 batches_scanned = 0;
  base::u64 bytes_truncated = 0;   // torn or corrupt tail thrown away
  base::u64 valid_bytes = 0;       // segment size after recovery
  base::u64 next_offset = 0;       // first offset the segment will hand out
  bool index_was_usable = false;   // false → the scan started from byte 0
  bool truncated = false;
};

// One `<base_offset>.log` file and its `.index` sidecar.
//
// A segment knows nothing about offset assignment; it stores framed batches whose
// offsets someone else chose, and refuses any batch that does not continue where the
// last one ended. Log owns the offset counter (§16.2).
class Segment {
 public:
  struct Options {
    base::u32 max_bytes = 32u * 1024u * 1024u;
    base::u32 index_interval_bytes = kDefaultIndexIntervalBytes;
  };

  // Opens the segment, creating it if absent, and recovers its tail: read the index,
  // scan forward from the last position it can vouch for, validate every batch, and
  // truncate at the first failure. A torn write at the tail is normal (FR-7), not an
  // error — the process died mid-pwrite and the bytes that survived are the bytes
  // that survived.
  static base::Result<std::unique_ptr<Segment>> open(io::Disk& disk, std::string_view dir,
                                                     base::u64 base_offset,
                                                     const Options& options);
  ~Segment();

  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

  // Appends already-framed bytes. `header` must be the batch's own decoded header —
  // the caller has it from BatchBuilder, so re-decoding here would be pure waste.
  base::Status append(base::Slice framed, const BatchHeader& header);

  // Durable when this returns. Note what it does NOT do: the index is deliberately
  // left unflushed, because the whole point of a rebuildable structure is not paying
  // to make it durable (§16.1).
  base::Status fsync();

  // Copies whole batches into `out`, starting with the batch that contains `offset`.
  // Returns the number of bytes written — always a whole number of batches, so the
  // caller can walk them without a partial-batch case to get wrong.
  //
  // Fails with kMessageTooLarge if the first matching batch alone does not fit: a
  // silent empty return would look identical to "end of log" to a consumer, and a
  // consumer that treats "your buffer is too small" as "you're caught up" stalls
  // forever.
  base::Result<std::size_t> read(base::u64 offset, base::MutSlice out) const;

  // Writes the index sidecar. Called at roll and at close — never fsynced.
  base::Status flush_index();

  [[nodiscard]] base::u64 base_offset() const noexcept { return base_offset_; }
  [[nodiscard]] base::u64 next_offset() const noexcept { return next_offset_; }
  [[nodiscard]] base::u32 size_bytes() const noexcept { return size_bytes_; }
  [[nodiscard]] bool empty() const noexcept { return size_bytes_ == 0; }
  [[nodiscard]] const SparseIndex& index() const noexcept { return index_; }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }

  // `00000000000000000000.log` — 20 digits, zero-padded, so lexicographic order is
  // numeric order and `ls` shows the log in the order it was written.
  static std::string log_name(base::u64 base_offset);
  static std::string index_name(base::u64 base_offset);
  // Parses a base offset back out of a `.log` name. Anything else in the directory is
  // not ours and is left alone.
  static base::Result<base::u64> parse_log_name(std::string_view name);

 private:
  Segment(io::Disk& disk, base::u64 base_offset, const Options& options)
      : disk_(disk), base_offset_(base_offset), next_offset_(base_offset), options_(options) {}

  base::Status recover();

  io::Disk& disk_;
  base::u64 base_offset_;
  base::u64 next_offset_;
  Options options_;

  io::FileId log_file_ = io::kInvalidFile;
  std::string log_path_;
  std::string index_path_;
  base::u32 size_bytes_ = 0;
  SparseIndex index_;
  RecoveryReport recovery_;
};

}  // namespace storage
