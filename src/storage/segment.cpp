#include "storage/segment.h"

#include <cassert>
#include <vector>

#include "base/buffer.h"

namespace storage {
namespace {

using base::ErrorCode;

// 20 digits holds any u64, and zero-padding makes lexicographic order numeric order —
// which is why list_directory() sorting by name is enough to recover segments in the
// order they were written.
constexpr std::size_t kOffsetDigits = 20;
constexpr std::string_view kLogSuffix = ".log";
constexpr std::string_view kIndexSuffix = ".index";

std::string offset_to_name(base::u64 offset, std::string_view suffix) {
  std::string name(kOffsetDigits, '0');
  base::u64 v = offset;
  for (std::size_t i = kOffsetDigits; i-- > 0 && v > 0;) {
    name[i] = static_cast<char>('0' + static_cast<char>(v % 10));
    v /= 10;
  }
  name.append(suffix);
  return name;
}

std::string join(std::string_view dir, std::string_view name) {
  std::string path(dir);
  if (!path.empty() && path.back() != '/') path.push_back('/');
  path.append(name);
  return path;
}

}  // namespace

std::string Segment::log_name(base::u64 base_offset) {
  return offset_to_name(base_offset, kLogSuffix);
}

std::string Segment::index_name(base::u64 base_offset) {
  return offset_to_name(base_offset, kIndexSuffix);
}

base::Result<base::u64> Segment::parse_log_name(std::string_view name) {
  if (name.size() != kOffsetDigits + kLogSuffix.size()) {
    return base::fail(ErrorCode::kInvalidArgument);
  }
  if (name.substr(kOffsetDigits) != kLogSuffix) return base::fail(ErrorCode::kInvalidArgument);

  base::u64 value = 0;
  for (std::size_t i = 0; i < kOffsetDigits; ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') return base::fail(ErrorCode::kInvalidArgument);
    value = value * 10 + static_cast<base::u64>(c - '0');
  }
  return value;
}

Segment::~Segment() {
  if (log_file_ != io::kInvalidFile) disk_.close(log_file_);
}

base::Result<std::unique_ptr<Segment>> Segment::open(io::Disk& disk, std::string_view dir,
                                                     base::u64 base_offset,
                                                     const Options& options) {
  std::unique_ptr<Segment> seg(new Segment(disk, base_offset, options));
  seg->log_path_ = join(dir, log_name(base_offset));
  seg->index_path_ = join(dir, index_name(base_offset));

  auto file = disk.open(seg->log_path_, io::OpenMode::kCreate);
  if (!file) return base::fail(file.error());
  seg->log_file_ = file.value();

  if (auto st = seg->recover(); !st) return base::fail(st.error());
  return seg;
}

base::Status Segment::recover() {
  auto size = disk_.size(log_file_);
  if (!size) return base::fail(size.error());
  if (size.value() > options_.max_bytes) {
    // A segment larger than its own bound means the file is not what we think it is.
    // Better to refuse than to silently index the first 32 MB of somebody else's data.
    return base::fail(ErrorCode::kCorruptRecord);
  }
  const auto file_size = static_cast<base::u32>(size.value());

  // Step 1: read the index, if there is one. It is never fsynced, so "absent",
  // "short", and "half an entry" are all normal — decode() drops what it cannot vouch
  // for and we scan the rest.
  if (auto index_file = disk_.open(index_path_, io::OpenMode::kRead); index_file) {
    const io::FileId id = index_file.value();
    if (auto index_size = disk_.size(id); index_size && index_size.value() > 0) {
      std::vector<base::u8> bytes(static_cast<std::size_t>(index_size.value()));
      auto got = disk_.pread(id, base::MutSlice(bytes.data(), bytes.size()), 0);
      if (got) {
        index_ = SparseIndex::decode(base::Slice(bytes.data(), got.value()), file_size);
      }
    }
    disk_.close(id);
  }

  // Step 2: pick the scan start. Every index entry is a claim that a valid batch
  // begins at that byte; verify the claim before trusting it, and walk backwards
  // until one holds. Usually none do after a crash — the index was never flushed —
  // and the scan starts at zero, which is exactly Kafka's behavior and the reason
  // recovery time is bounded by segment size rather than log size.
  base::u32 scan_pos = 0;
  base::u64 expect_offset = base_offset_;
  std::vector<base::u8> scratch;

  while (!index_.empty()) {
    const IndexEntry candidate = index_[index_.size() - 1];
    bool usable = false;

    base::u8 header_bytes[kBatchHeaderBytes];
    auto got = disk_.pread(log_file_, base::MutSlice(header_bytes, kBatchHeaderBytes),
                           candidate.file_pos);
    if (got && got.value() == kBatchHeaderBytes) {
      auto header = decode_header(base::Slice(header_bytes, kBatchHeaderBytes));
      if (header && header.value().base_offset == base_offset_ + candidate.rel_offset) {
        usable = true;
      }
    }

    if (usable) {
      scan_pos = candidate.file_pos;
      expect_offset = base_offset_ + candidate.rel_offset;
      recovery_.index_was_usable = true;
      break;
    }
    index_.truncate_from(candidate.file_pos);
  }

  // Step 3: scan forward, validating every batch, and stop at the first thing that
  // does not check out. Everything from there on is a torn write, and a torn write at
  // the tail is normal (FR-7) — logged at info in the real runtime, never an error.
  // A read that *fails* and a read that comes up *short* mean opposite things here, and
  // conflating them destroys data. A short read is end-of-file: the tail was torn, the
  // bytes are genuinely not there, truncate. An I/O error is the disk declining to
  // answer — the bytes may be perfectly fine — and truncating on it would delete
  // durable, acked records because a read hiccuped once. Refusing to open the segment
  // is the only safe response: the node stays down, retries, and loses nothing.
  //
  // Found by the simulator on seed 1 with `--io-errors 0.02`; see
  // `docs/retrospective.md` §1 entry #1.
  base::u32 pos = scan_pos;
  while (pos < file_size) {
    base::u8 header_bytes[kBatchHeaderBytes];
    auto got = disk_.pread(log_file_, base::MutSlice(header_bytes, kBatchHeaderBytes), pos);
    if (!got) return base::fail(got.error());
    if (got.value() < kBatchHeaderBytes) break;  // short tail

    auto header = decode_header(base::Slice(header_bytes, kBatchHeaderBytes));
    if (!header) break;
    const BatchHeader& h = header.value();

    const std::size_t total = h.total_bytes();
    if (total > file_size - pos) break;  // the batch claims more bytes than survived

    scratch.resize(total);
    auto body = disk_.pread(log_file_, base::MutSlice(scratch.data(), total), pos);
    if (!body) return base::fail(body.error());
    if (body.value() < total) break;
    if (!validate_batch(base::Slice(scratch.data(), total))) break;

    // Offsets must continue exactly where the previous batch ended. A batch that is
    // internally valid but starts at the wrong offset is not a torn write — it is a
    // different log, or the same log written twice, and either way stopping here is
    // the only safe move.
    if (h.base_offset != expect_offset) break;

    index_.maybe_add(static_cast<base::u32>(h.base_offset - base_offset_), pos,
                     options_.index_interval_bytes);
    expect_offset = h.next_offset();
    pos += static_cast<base::u32>(total);
    ++recovery_.batches_scanned;
  }

  if (pos < file_size) {
    if (auto st = disk_.truncate(log_file_, pos); !st) return base::fail(st.error());
    index_.truncate_from(pos);
    recovery_.truncated = true;
    recovery_.bytes_truncated = file_size - pos;
  }

  size_bytes_ = pos;
  next_offset_ = expect_offset;
  recovery_.valid_bytes = pos;
  recovery_.next_offset = expect_offset;
  return {};
}

base::Status Segment::append(base::Slice framed, const BatchHeader& header) {
  if (header.base_offset != next_offset_) return base::fail(ErrorCode::kInvalidArgument);
  if (framed.size() > options_.max_bytes - size_bytes_) {
    return base::fail(ErrorCode::kMessageTooLarge);
  }

  auto written = disk_.pwrite(log_file_, framed, size_bytes_);
  if (!written) return base::fail(written.error());
  if (written.value() != framed.size()) return base::fail(ErrorCode::kIoError);

  index_.maybe_add(static_cast<base::u32>(header.base_offset - base_offset_), size_bytes_,
                   options_.index_interval_bytes);
  size_bytes_ += static_cast<base::u32>(framed.size());
  next_offset_ = header.next_offset();
  return {};
}

base::Status Segment::fsync() { return disk_.fsync(log_file_); }

base::Result<std::size_t> Segment::read(base::u64 offset, base::MutSlice out) const {
  if (offset >= next_offset_) return std::size_t{0};
  const base::u64 want = offset < base_offset_ ? base_offset_ : offset;

  // The index bounds the scan; it never answers it. Start at the nearest batch it
  // knows about and walk forward to the batch that actually contains `want`.
  base::u32 pos = index_.lookup(static_cast<base::u32>(want - base_offset_)).file_pos;
  std::size_t first_total = 0;

  while (pos < size_bytes_) {
    base::u8 header_bytes[kBatchHeaderBytes];
    auto got = disk_.pread(log_file_, base::MutSlice(header_bytes, kBatchHeaderBytes), pos);
    // Same distinction as recovery, for the same reason one level down: a disk that
    // declined to answer has not told us the data is bad. Reporting kCorruptRecord —
    // a *terminal* error — would make a client give up permanently on records that are
    // intact, when the honest answer is "try again".
    if (!got) return base::fail(got.error());
    if (got.value() < kBatchHeaderBytes) return base::fail(ErrorCode::kCorruptRecord);

    auto header = decode_header(base::Slice(header_bytes, kBatchHeaderBytes));
    if (!header) return base::fail(ErrorCode::kCorruptRecord);

    if (header.value().next_offset() > want) {
      first_total = header.value().total_bytes();
      break;
    }
    pos += static_cast<base::u32>(header.value().total_bytes());
  }
  if (pos >= size_bytes_) return std::size_t{0};

  if (out.size() < first_total) return base::fail(ErrorCode::kMessageTooLarge);

  const std::size_t avail = size_bytes_ - pos;
  const std::size_t want_bytes = avail < out.size() ? avail : out.size();
  auto got = disk_.pread(log_file_, out.subslice(0, want_bytes), pos);
  if (!got) return base::fail(got.error());

  // Hand back whole batches only, and validate every one of them. The bulk read stops
  // wherever the buffer ran out, which is usually the middle of a batch; a caller that
  // received half of one would have to re-derive the boundary the reader already knows.
  //
  // The CRC check here is not redundant with recovery. Recovery validates the segment
  // once, at open; a bit that flips afterwards — §14.1 lists silent corruption as a
  // first-class fault — is never seen again on any other path. §17 is unambiguous about
  // what to do: CRC on read, refuse to serve. Returning wrong bytes with an ok status
  // is the one failure this layer must never produce.
  //
  // It costs a CRC pass over every byte served, and that is the honest price of the
  // guarantee. It is also why week 7's zero-copy fetch is a real trade-off rather than
  // a free win: sending straight from the page cache means nobody checksums the bytes
  // on the way out. That decision gets made with a benchmark, not by default.
  std::size_t whole = 0;
  while (whole + kBatchHeaderBytes <= got.value()) {
    const base::Slice candidate = out.as_slice().subslice(whole, got.value() - whole);
    auto header = decode_header(candidate);
    if (!header) return base::fail(ErrorCode::kCorruptRecord);

    const std::size_t total = header.value().total_bytes();
    if (whole + total > got.value()) break;
    if (!validate_batch(candidate)) return base::fail(ErrorCode::kCorruptRecord);
    whole += total;
  }
  return whole;
}

base::Status Segment::scan_headers(
    const std::function<bool(const BatchHeader&)>& visit) const {
  base::u32 pos = 0;
  while (pos < size_bytes_) {
    base::u8 header_bytes[kBatchHeaderBytes];
    auto got = disk_.pread(log_file_, base::MutSlice(header_bytes, kBatchHeaderBytes), pos);
    if (!got) return base::fail(got.error());
    if (got.value() < kBatchHeaderBytes) return base::fail(ErrorCode::kCorruptRecord);

    auto header = decode_header(base::Slice(header_bytes, kBatchHeaderBytes));
    if (!header) return base::fail(ErrorCode::kCorruptRecord);

    if (!visit(header.value())) return {};
    pos += static_cast<base::u32>(header.value().total_bytes());
  }
  return {};
}

base::Status Segment::truncate_to(base::u64 offset) {
  if (offset >= next_offset_) return {};  // nothing of ours is past it

  // Walk to the batch that begins at `offset`. The index bounds the scan the same way it
  // does on the read path — it never answers the question, it only says where to start.
  base::u32 pos = 0;
  if (offset > base_offset_) {
    pos = index_.lookup(static_cast<base::u32>(offset - base_offset_)).file_pos;
  }

  base::u32 cut = pos;
  bool found = offset <= base_offset_;
  while (!found && pos < size_bytes_) {
    base::u8 header_bytes[kBatchHeaderBytes];
    auto got = disk_.pread(log_file_, base::MutSlice(header_bytes, kBatchHeaderBytes), pos);
    if (!got) return base::fail(got.error());
    if (got.value() < kBatchHeaderBytes) return base::fail(ErrorCode::kCorruptRecord);

    auto header = decode_header(base::Slice(header_bytes, kBatchHeaderBytes));
    if (!header) return base::fail(ErrorCode::kCorruptRecord);

    if (header.value().base_offset == offset) {
      cut = pos;
      found = true;
      break;
    }
    // Cutting into the middle of a batch would leave a record half-present, which is a
    // worse state than either keeping or dropping the whole thing. Raft only ever names
    // a batch boundary, so if we walked past one the caller is confused.
    if (header.value().base_offset > offset) return base::fail(ErrorCode::kInvalidArgument);
    pos += static_cast<base::u32>(header.value().total_bytes());
  }
  if (!found) return base::fail(ErrorCode::kInvalidArgument);

  if (auto st = disk_.truncate(log_file_, cut); !st) return base::fail(st.error());
  size_bytes_ = cut;
  next_offset_ = offset;
  index_.truncate_from(cut);
  return {};
}

base::Status Segment::flush_index() {
  base::Buffer out;
  index_.encode(out);

  auto file = disk_.open(index_path_, io::OpenMode::kCreate);
  if (!file) return base::fail(file.error());
  const io::FileId id = file.value();

  // Truncate-then-write is not atomic, and deliberately so: this file is never
  // fsynced and never trusted (§16.1). Losing it costs a scan on the next open, which
  // is a price worth paying to keep it off the durability path entirely.
  base::Status st = disk_.truncate(id, 0);
  if (st && !out.empty()) {
    auto written = disk_.pwrite(id, out.slice(), 0);
    if (!written) st = base::fail(written.error());
  }
  disk_.close(id);
  return st;
}

}  // namespace storage
