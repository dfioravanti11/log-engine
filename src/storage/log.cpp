#include "storage/log.h"

#include <algorithm>

namespace storage {
namespace {

using base::ErrorCode;

std::string join(std::string_view dir, std::string_view name) {
  std::string path(dir);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(name);
  return path;
}

}  // namespace

Log::~Log() {
  // Best effort: the index is a hint, and failing to write a hint on the way out is
  // not worth a crash. The next open() pays for it with a scan.
  (void)flush_indexes();
}

base::Result<std::unique_ptr<Log>> Log::open(io::Disk& disk, std::string_view dir,
                                             const Options& options) {
  if (auto st = disk.make_directories(dir); !st) return base::fail(st.error());

  std::unique_ptr<Log> log(new Log(disk, dir, options));
  if (auto st = log->recover(); !st) return base::fail(st.error());
  return log;
}

base::Result<std::vector<base::u64>> Log::existing_segment_offsets() const {
  auto names = disk_.list_directory(dir_);
  if (!names) return base::fail(names.error());

  std::vector<base::u64> offsets;
  for (const std::string& name : names.value()) {
    // Anything that is not a segment file belongs to somebody else — raft.state, the
    // index sidecars, an editor's swap file. Skip it rather than fail the open.
    if (auto parsed = Segment::parse_log_name(name); parsed) offsets.push_back(parsed.value());
  }
  std::sort(offsets.begin(), offsets.end());
  return offsets;
}

base::Status Log::add_segment(base::u64 base_offset) {
  Segment::Options seg_options;
  seg_options.max_bytes = options_.segment_max_bytes;
  seg_options.index_interval_bytes = options_.index_interval_bytes;

  auto seg = Segment::open(disk_, dir_, base_offset, seg_options);
  if (!seg) return base::fail(seg.error());
  segments_.push_back(std::move(seg).value());
  return {};
}

void Log::remove_segment_files(base::u64 base_offset) {
  (void)disk_.remove(join(dir_, Segment::log_name(base_offset)));
  (void)disk_.remove(join(dir_, Segment::index_name(base_offset)));
}

base::Status Log::recover() {
  auto offsets = existing_segment_offsets();
  if (!offsets) return base::fail(offsets.error());
  const std::vector<base::u64>& bases = offsets.value();

  std::size_t kept = 0;
  for (; kept < bases.size(); ++kept) {
    // A segment that does not start exactly where the previous one ended is a hole,
    // and a hole in an append-only log is not recoverable — the offsets after it can
    // never be reached by a sequential read. Everything from here on gets dropped.
    if (!segments_.empty() && bases[kept] != segments_.back()->next_offset()) break;

    if (auto st = add_segment(bases[kept]); !st) return base::fail(st.error());
    const RecoveryReport& r = segments_.back()->recovery();
    recovery_.segments_opened++;
    recovery_.batches_scanned += r.batches_scanned;
    recovery_.bytes_truncated += r.bytes_truncated;
    recovery_.truncated = recovery_.truncated || r.truncated;

    // If this segment lost its tail, every later segment is unreachable for the same
    // reason. This is the case that only shows up when a crash lands during a roll,
    // which is exactly the window the week-2 demo aims at.
    if (r.truncated) {
      ++kept;
      break;
    }
  }

  for (std::size_t i = kept; i < bases.size(); ++i) {
    remove_segment_files(bases[i]);
    recovery_.segments_discarded++;
  }

  if (segments_.empty()) {
    if (auto st = add_segment(0); !st) return base::fail(st.error());
    recovery_.segments_opened++;
  }

  start_offset_ = segments_.front()->base_offset();
  next_offset_ = segments_.back()->next_offset();
  return {};
}

base::Status Log::roll() {
  Segment& active = *segments_.back();

  // fsync before the roll, so recovery only ever has to distrust the last segment.
  // Without this, a crash could leave a torn write in the middle of the log and the
  // scan would have to check every segment on every open.
  if (auto st = active.fsync(); !st) return base::fail(st.error());
  if (auto st = active.flush_index(); !st) return base::fail(st.error());
  return add_segment(next_offset_);
}

base::Result<base::u64> Log::append(BatchBuilder& builder, const BatchMeta& meta) {
  if (builder.empty()) return base::fail(ErrorCode::kInvalidRequest);
  if (builder.framed_bytes() > options_.segment_max_bytes) {
    return base::fail(ErrorCode::kMessageTooLarge);
  }

  Segment* active = segments_.back().get();
  // An empty segment takes the batch regardless: rolling to a second empty segment
  // would loop forever, and a batch larger than the segment bound was already rejected.
  if (!active->empty() &&
      builder.framed_bytes() > options_.segment_max_bytes - active->size_bytes()) {
    if (auto st = roll(); !st) return base::fail(st.error());
    active = segments_.back().get();
  }

  const base::u64 base_offset = next_offset_;
  const base::Slice framed = builder.finish(base_offset, meta);

  auto header = decode_header(framed);
  if (!header) return base::fail(header.error());

  if (auto st = active->append(framed, header.value()); !st) return base::fail(st.error());
  next_offset_ = header.value().next_offset();
  return base_offset;
}

base::Status Log::fsync() { return segments_.back()->fsync(); }

base::Result<std::size_t> Log::read(base::u64 offset, base::MutSlice out) const {
  if (offset >= next_offset_) return std::size_t{0};
  if (offset < start_offset_) return base::fail(ErrorCode::kOffsetOutOfRange);

  // Segments are sorted by base offset, so the owning one is the last whose base is
  // <= offset. The loop after it exists for one case: the owning segment ends exactly
  // at `offset`, and the data lives at the start of the next one.
  auto it = std::upper_bound(segments_.begin(), segments_.end(), offset,
                             [](base::u64 target, const std::unique_ptr<Segment>& s) {
                               return target < s->base_offset();
                             });
  std::size_t i = static_cast<std::size_t>(it - segments_.begin());
  if (i > 0) --i;

  for (; i < segments_.size(); ++i) {
    auto got = segments_[i]->read(offset, out);
    if (!got) return base::fail(got.error());
    if (got.value() > 0) return got.value();
  }
  return std::size_t{0};
}

base::Status Log::flush_indexes() {
  base::Status result;
  for (const auto& seg : segments_) {
    if (auto st = seg->flush_index(); !st) result = base::fail(st.error());
  }
  return result;
}

}  // namespace storage
