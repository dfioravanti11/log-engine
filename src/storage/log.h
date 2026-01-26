#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "io/disk.h"
#include "storage/record_batch.h"
#include "storage/segment.h"

namespace storage {

// One run of consecutive offsets written by a single leader — the leader epoch cache of
// §12.3. Storage-level on purpose: `storage/` must not depend on `raft/`, so the term is
// a `u32 epoch` here and `raft::Epoch` upstream.
struct EpochBoundary {
  base::u32 epoch = 0;
  base::u64 start_offset = 0;
};

// The whole recovery story for one partition directory, aggregated across segments.
struct LogRecoveryReport {
  base::u64 segments_opened = 0;
  base::u64 segments_discarded = 0;  // unreachable after a truncation earlier in the log
  base::u64 batches_scanned = 0;
  base::u64 bytes_truncated = 0;
  bool truncated = false;
};

// One partition's log: an ordered run of segments in one directory.
//
// Log is the **offset authority**. Segments store batches and refuse any that does not
// continue where the last one ended; nothing else in the system may choose an offset.
// Keeping that in one place is what makes "offsets are monotonic" (I2) a property of
// one counter rather than a property everybody has to remember.
//
// No commit index here, and no fetch clamping. Those belong to Raft (§12.1) and arrive
// in week 4 — a fetch that reads up to next_offset is correct for a single node and
// would be badly wrong for a replicated one.
class Log {
 public:
  struct Options {
    // 32 MB, not Kafka's 128 MB. Deliberate: a benchmark window that never rolls a
    // segment never exercises rolling, and untested code on the durability path is
    // where the bugs live (`project_spec.md` §25 Q1 — this answers it for now).
    base::u32 segment_max_bytes = 32u * 1024u * 1024u;
    base::u32 index_interval_bytes = kDefaultIndexIntervalBytes;
  };

  static base::Result<std::unique_ptr<Log>> open(io::Disk& disk, std::string_view dir,
                                                 const Options& options);
  static base::Result<std::unique_ptr<Log>> open(io::Disk& disk, std::string_view dir) {
    return open(disk, dir, Options{});
  }
  ~Log();

  Log(const Log&) = delete;
  Log& operator=(const Log&) = delete;

  // Frames the builder's records at the next offset and writes them. Returns the base
  // offset assigned. The batch is in the page cache when this returns — durable only
  // after fsync(), which is the entire distinction `acks` is built on (§13).
  base::Result<base::u64> append(BatchBuilder& builder, const BatchMeta& meta);

  // ---- The replication path (week 5) ----
  //
  // A follower does not get to choose offsets. The batch arriving from the leader already
  // carries the offset the leader assigned, and it must land at exactly that offset with
  // exactly those bytes — re-framing it here would change the CRC and break the identity
  // that makes "these two nodes hold the same entry" checkable at all.
  //
  // So this is `append()` with the offset decision removed rather than delegated: the
  // batch's own `base_offset` must equal `next_offset()`, and it is rejected otherwise.
  // `Log` remains the offset authority for everything the *client* writes (I2); this is
  // the one path where the authority sits upstream, in the leader.
  base::Status append_replicated(base::Slice framed);

  // Drops everything from `offset` onward. Used when a follower's tail diverged from the
  // leader's (Raft §5.3) — those entries were never committed, by construction, because
  // a committed entry cannot be missing from a leader's log (§5.4.1).
  //
  // Fails with kInvalidArgument if `offset` is below `start_offset()` or above
  // `next_offset()`: truncating into reclaimed history, or past the end, is a bug in the
  // caller and not something to paper over.
  base::Status truncate_to(base::u64 offset);

  // The leader epoch cache (§12.3, §16.1): one entry per change of `leader_epoch`, in
  // offset order, rebuilt by scanning batch headers.
  //
  // Rebuilt rather than persisted, on purpose — it is derivable from the log, and a
  // second durable structure is a second thing that can be wrong in a way the first one
  // disagrees with. Raft needs it to answer "what term produced the entry at N?" without
  // holding the entries.
  [[nodiscard]] base::Result<std::vector<EpochBoundary>> scan_epochs() const;

  // Durable when this returns, for everything appended so far.
  base::Status fsync();

  // Whole batches starting from the batch containing `offset`. Returns 0 bytes when
  // the reader is caught up.
  base::Result<std::size_t> read(base::u64 offset, base::MutSlice out) const;

  // Writes every segment's index sidecar. Called by the destructor; exposed so a
  // clean shutdown can do it explicitly and check the result.
  base::Status flush_indexes();

  [[nodiscard]] base::u64 start_offset() const noexcept { return start_offset_; }
  [[nodiscard]] base::u64 next_offset() const noexcept { return next_offset_; }
  [[nodiscard]] std::size_t segment_count() const noexcept { return segments_.size(); }
  [[nodiscard]] const Segment& segment(std::size_t i) const { return *segments_[i]; }
  [[nodiscard]] const LogRecoveryReport& recovery() const noexcept { return recovery_; }

 private:
  Log(io::Disk& disk, std::string_view dir, const Options& options)
      : disk_(disk), dir_(dir), options_(options) {}

  base::Status recover();
  base::Status roll();
  base::Result<std::vector<base::u64>> existing_segment_offsets() const;
  base::Status add_segment(base::u64 base_offset);
  void remove_segment_files(base::u64 base_offset);

  io::Disk& disk_;
  std::string dir_;
  Options options_;

  std::vector<std::unique_ptr<Segment>> segments_;
  base::u64 start_offset_ = 0;
  base::u64 next_offset_ = 0;
  LogRecoveryReport recovery_;
};

}  // namespace storage
