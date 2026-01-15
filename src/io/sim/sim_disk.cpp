#include "io/sim/sim_disk.h"

#include <algorithm>
#include <cstring>

namespace io::sim {
namespace {

using base::ErrorCode;

// This disk holds its files in RAM and cannot be sparse, so a wild offset would try to
// allocate itself for real. A cap turns "the caller computed a garbage offset" into an
// error instead of an OOM kill halfway through a simulation run.
constexpr base::u64 kMaxFileBytes = base::u64{1} << 30;

// Trailing slashes are noise: storage/ joins paths by hand and "dir" and "dir/" must
// name the same directory or list_directory() silently returns nothing.
std::string normalize(std::string_view path) {
  std::string p(path);
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  return p;
}

std::string parent_of(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  if (slash == std::string::npos) return "";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

std::string basename_of(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

void apply_bytes(std::vector<base::u8>& image, base::u64 offset, const base::u8* data,
                 std::size_t count) {
  if (count == 0) return;
  const std::size_t start = static_cast<std::size_t>(offset);
  if (image.size() < start + count) image.resize(start + count, 0);  // past EOF zero-fills
  std::memcpy(image.data() + start, data, count);
}

}  // namespace

SimDisk::SimDisk(Random& rng, DiskFaultConfig faults) : rng_(rng), faults_(faults) {
  dirs_.insert("");   // the root of a relative path tree
  dirs_.insert("/");  // and of an absolute one
}

SimDisk::File* SimDisk::resolve(FileId file, bool for_write, ErrorCode* err) {
  // One gate covering every read and write path: between the power cut and the reboot
  // the disk does nothing at all, and that includes whatever a dying process attempts
  // on its way down.
  if (!powered_) {
    *err = ErrorCode::kIoError;
    return nullptr;
  }
  const auto handle = open_.find(file);
  if (handle == open_.end()) {
    *err = ErrorCode::kNotFound;
    return nullptr;
  }
  if (for_write && handle->second.mode == OpenMode::kRead) {
    *err = ErrorCode::kIoError;  // what a real pwrite on an O_RDONLY fd gets (EBADF)
    return nullptr;
  }
  const auto entry = files_.find(handle->second.path);
  if (entry == files_.end()) {
    // Unlinked while open. POSIX would keep the inode alive until the last close; this
    // model does not, because nothing in storage/ reads a file it has already removed.
    *err = ErrorCode::kNotFound;
    return nullptr;
  }
  return &entry->second;
}

base::Result<FileId> SimDisk::open(std::string_view path, OpenMode mode) {
  if (!powered_) return base::fail(ErrorCode::kIoError);
  const std::string p = normalize(path);
  if (dirs_.count(p) != 0) return base::fail(ErrorCode::kIoError);

  auto entry = files_.find(p);
  if (entry == files_.end()) {
    if (mode != OpenMode::kCreate) return base::fail(ErrorCode::kNotFound);
    // O_CREAT does not create the parent directory, and a Log that forgot to call
    // make_directories() should fail here rather than in some later recovery scan.
    if (dirs_.count(parent_of(p)) == 0) return base::fail(ErrorCode::kNotFound);
    entry = files_.emplace(p, File{}).first;  // kCreate never truncates
  }

  const FileId id = next_id_++;
  open_.emplace(id, Handle{p, mode});
  return id;
}

void SimDisk::close(FileId file) { open_.erase(file); }

base::Result<std::size_t> SimDisk::pwrite(FileId file, base::Slice data, base::u64 offset) {
  ErrorCode err = ErrorCode::kNone;
  File* f = resolve(file, /*for_write=*/true, &err);
  if (f == nullptr) return base::fail(err);

  // All or nothing. A half-applied write is a torn write, which is crash()'s business,
  // not this knob's — conflating the two would make torn tails appear without a crash.
  if (rng_.next_bool_with_probability(faults_.io_error_probability)) {
    return base::fail(ErrorCode::kIoError);
  }
  if (data.empty()) return static_cast<std::size_t>(0);

  const base::u64 end = offset + static_cast<base::u64>(data.size());
  if (end < offset || end > kMaxFileBytes) return base::fail(ErrorCode::kInvalidArgument);

  apply_bytes(f->visible, offset, data.data(), data.size());
  f->pending.push_back(PendingWrite{offset, std::vector<base::u8>(data.begin(), data.end())});
  return data.size();
}

base::Result<std::size_t> SimDisk::pread(FileId file, base::MutSlice out, base::u64 offset) {
  ErrorCode err = ErrorCode::kNone;
  File* f = resolve(file, /*for_write=*/false, &err);
  if (f == nullptr) return base::fail(err);

  if (rng_.next_bool_with_probability(faults_.io_error_probability)) {
    return base::fail(ErrorCode::kIoError);
  }

  // EOF is a short read, not an error: the tail scan in §16.2 walks off the end of a
  // torn segment on purpose and treats that as the stopping condition.
  if (offset >= f->visible.size()) return static_cast<std::size_t>(0);
  const std::size_t start = static_cast<std::size_t>(offset);
  const std::size_t count = std::min(out.size(), f->visible.size() - start);
  if (count > 0) std::memcpy(out.data(), f->visible.data() + start, count);
  return count;
}

base::Status SimDisk::fsync(FileId file) {
  ErrorCode err = ErrorCode::kNone;
  File* f = resolve(file, /*for_write=*/false, &err);
  if (f == nullptr) return base::fail(err);

  // A failed fsync leaves the pending list alone: the bytes did not reach the platter,
  // so a crash may still take them. Clearing it here is the bug that makes a simulator
  // agree with a broken implementation.
  if (rng_.next_bool_with_probability(faults_.io_error_probability)) {
    return base::fail(ErrorCode::kIoError);
  }

  // Replaying the pending list onto the durable image, rather than copying the whole
  // visible image across. The two are equivalent — `durable + pending == visible` holds
  // by construction, and truncate() maintains it on both images at once — but the copy
  // costs O(file size) per fsync, so a log that fsyncs every append pays quadratically
  // in the segment it is filling.
  //
  // That is not a micro-optimization. Simulation speed is a requirement (NFR-4), and a
  // simulator too slow to run a thousand seeds finds a thousand times fewer bugs.
  for (const PendingWrite& write : f->pending) {
    apply_bytes(f->durable, write.offset, write.bytes.data(), write.bytes.size());
  }
  f->pending.clear();
  return {};
}

base::Status SimDisk::truncate(FileId file, base::u64 size) {
  ErrorCode err = ErrorCode::kNone;
  File* f = resolve(file, /*for_write=*/true, &err);
  if (f == nullptr) return base::fail(err);
  if (size > kMaxFileBytes) return base::fail(ErrorCode::kInvalidArgument);

  const std::size_t end = static_cast<std::size_t>(size);
  f->visible.resize(end, 0);
  f->durable.resize(end, 0);

  // Pending writes past the new end go with the bytes they targeted, and one that
  // straddles it keeps only the part still inside the file. Recovery truncates a torn
  // tail and has to be able to mean it.
  for (PendingWrite& w : f->pending) {
    if (w.offset >= size) {
      w.bytes.clear();
    } else if (static_cast<base::u64>(w.bytes.size()) > size - w.offset) {
      w.bytes.resize(static_cast<std::size_t>(size - w.offset));
    }
  }
  f->pending.erase(std::remove_if(f->pending.begin(), f->pending.end(),
                                  [](const PendingWrite& w) { return w.bytes.empty(); }),
                   f->pending.end());
  return {};
}

base::Result<base::u64> SimDisk::size(FileId file) {
  ErrorCode err = ErrorCode::kNone;
  File* f = resolve(file, /*for_write=*/false, &err);
  if (f == nullptr) return base::fail(err);
  return static_cast<base::u64>(f->visible.size());
}

base::Status SimDisk::remove(std::string_view path) {
  if (!powered_) return base::fail(ErrorCode::kIoError);
  const std::string p = normalize(path);
  const auto entry = files_.find(p);
  if (entry == files_.end()) {
    return base::fail(dirs_.count(p) != 0 ? ErrorCode::kIoError : ErrorCode::kNotFound);
  }
  files_.erase(entry);
  return {};
}

base::Status SimDisk::make_directories(std::string_view path) {
  if (!powered_) return base::fail(ErrorCode::kIoError);
  const std::string p = normalize(path);
  if (p.empty()) return {};  // the root is always there

  for (std::size_t i = 0; i <= p.size(); ++i) {
    if (i != p.size() && p[i] != '/') continue;
    const std::string prefix = p.substr(0, i == 0 ? 1 : i);
    if (files_.count(prefix) != 0) return base::fail(ErrorCode::kIoError);
    dirs_.insert(prefix);
  }
  return {};
}

base::Result<std::vector<std::string>> SimDisk::list_directory(std::string_view path) {
  if (!powered_) return base::fail(ErrorCode::kIoError);
  const std::string p = normalize(path);
  if (dirs_.count(p) == 0) {
    return base::fail(files_.count(p) != 0 ? ErrorCode::kIoError : ErrorCode::kNotFound);
  }

  // A set, so the result is sorted by construction — see the io::Disk contract for why
  // recovery depends on it.
  std::set<std::string> names;
  for (const auto& entry : files_) {
    if (parent_of(entry.first) == p) names.insert(basename_of(entry.first));
  }
  for (const std::string& dir : dirs_) {
    if (!dir.empty() && dir != p && parent_of(dir) == p) names.insert(basename_of(dir));
  }
  return std::vector<std::string>(names.begin(), names.end());
}

void SimDisk::crash() {
  ++crashes_;

  // The two knobs are marginal probabilities. Reaching the tear branch already means
  // the keep draw failed, so the tear draw is rescaled rather than double-counted.
  const double keep_p = faults_.keep_unflushed_probability;
  const double tear_p =
      keep_p >= 1.0 ? 0.0 : faults_.torn_write_probability / (1.0 - keep_p);

  for (auto& entry : files_) {
    File& f = entry.second;
    f.visible = f.durable;

    for (std::size_t i = 0; i < f.pending.size(); ++i) {
      const PendingWrite& w = f.pending[i];
      const std::size_t total = w.bytes.size();

      std::size_t applied = 0;
      if (rng_.next_bool_with_probability(keep_p)) {
        applied = total;
      } else if (rng_.next_bool_with_probability(tear_p) && total > 1) {
        // A random non-empty proper prefix. A one-byte write has none, so tearing it
        // degenerates to dropping it.
        applied = 1 + static_cast<std::size_t>(rng_.next_below(total - 1));
      }

      if (applied > 0) apply_bytes(f.visible, w.offset, w.bytes.data(), applied);
      bytes_lost_ += static_cast<base::u64>(total - applied);

      if (applied < total) {
        // Stop. A page cache does not lose a write from the middle and keep the ones
        // issued after it, so a model that did would manufacture failures no real disk
        // produces — and send whoever debugs the resulting trace chasing a bug that
        // cannot happen.
        for (std::size_t j = i + 1; j < f.pending.size(); ++j) {
          bytes_lost_ += static_cast<base::u64>(f.pending[j].bytes.size());
        }
        break;
      }
    }

    f.pending.clear();
    f.durable = f.visible;  // rebooted: what is on the platter is what is there
  }

  powered_ = false;

  // Every FileId belonged to a process that no longer exists. next_id_ keeps climbing,
  // so a stale id can never be mistaken for a live one after restart.
  open_.clear();
}

bool SimDisk::corrupt_random_byte() {
  // Only files with something on the platter can rot. An unflushed write lives in the
  // page cache, where a bit-flip is a different fault — and one a crash would erase.
  // Collected in std::map order, so the pointers are ordered by path, never by address.
  std::vector<File*> candidates;
  for (auto& entry : files_) {
    if (!entry.second.durable.empty()) candidates.push_back(&entry.second);
  }
  if (candidates.empty()) return false;

  File& f = *candidates[static_cast<std::size_t>(rng_.next_below(candidates.size()))];
  const std::size_t index = static_cast<std::size_t>(rng_.next_below(f.durable.size()));
  const base::u8 mask = static_cast<base::u8>(1u << rng_.next_below(8));
  const base::u8 before = f.durable[index];
  f.durable[index] = static_cast<base::u8>(before ^ mask);

  // Mirror it into the visible image, or no reader could ever see the rot and the
  // fault would be untestable — unless a pending write already replaced that byte, in
  // which case the reader is looking at newer bytes and the damage is not yet visible.
  if (index < f.visible.size() && f.visible[index] == before) {
    f.visible[index] = f.durable[index];
  }
  return true;
}

base::u64 SimDisk::unflushed_bytes() const {
  base::u64 total = 0;
  for (const auto& entry : files_) {
    for (const PendingWrite& w : entry.second.pending) {
      total += static_cast<base::u64>(w.bytes.size());
    }
  }
  return total;
}

}  // namespace io::sim
