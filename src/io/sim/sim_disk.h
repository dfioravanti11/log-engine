#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "io/disk.h"
#include "io/random.h"

namespace io::sim {

struct DiskFaultConfig {
  // On crash, the chance an unflushed write is applied only partially rather than
  // kept whole or dropped outright.
  double torn_write_probability = 0.15;
  // On crash, the chance an unflushed write survives at all.
  double keep_unflushed_probability = 0.30;
  // Per-operation chance pwrite/pread/fsync returns kIoError.
  double io_error_probability = 0.0;
};

// An in-memory filesystem for one simulated node.
//
// Every file carries two images: the **durable** bytes — the platter, as of the last
// successful fsync — and the **visible** bytes, which is what a pread returns right
// now. pwrite moves only the visible image and queues the write; fsync is the only
// thing that makes the two agree. That split is the entire reason this class exists:
// it is what makes §13's two durability knobs mean different things, and what lets
// crash() take away exactly the writes a power cut would take away.
//
// File *metadata* — creation, remove(), make_directories(), truncate() — is modeled as
// synchronously durable; only file contents take part in the split. Real filesystems
// can lose metadata too, but that fault produces failures storage/ has no way to
// defend against, and the durability argument this class supports is about record
// bytes.
class SimDisk final : public Disk {
 public:
  explicit SimDisk(Random& rng, DiskFaultConfig faults = {});

  SimDisk(const SimDisk&) = delete;
  SimDisk& operator=(const SimDisk&) = delete;

  base::Result<FileId> open(std::string_view path, OpenMode mode) override;
  void close(FileId file) override;
  base::Result<std::size_t> pwrite(FileId file, base::Slice data, base::u64 offset) override;
  base::Result<std::size_t> pread(FileId file, base::MutSlice out, base::u64 offset) override;
  base::Status fsync(FileId file) override;
  base::Status truncate(FileId file, base::u64 size) override;
  base::Result<base::u64> size(FileId file) override;
  base::Status remove(std::string_view path) override;
  base::Status make_directories(std::string_view path) override;
  base::Result<std::vector<std::string>> list_directory(std::string_view path) override;

  // ---- Fault surface: the simulator calls these, nothing above the seam ever does. ----

  // A power cut. Each file keeps its durable bytes plus whatever survives of its
  // unflushed writes, and the result becomes the new durable state — the machine
  // rebooted, and what is on the platter is what is there. Also invalidates every open
  // FileId, because the process that held them is gone.
  //
  // Leaves the disk **powered off**: every operation fails with kIoError until
  // power_on(). Without that the dying process gets a last word it would never get in
  // reality — C++ still runs its destructors, and `storage::Log::~Log()` writes the
  // sparse index on the way out. A crash that politely flushes an index is not a crash,
  // and recovery would then be tested against a tidier disk than a real one leaves.
  void crash();

  // The machine comes back up.
  void power_on() { powered_ = true; }
  [[nodiscard]] bool powered() const { return powered_; }

  // §14.1's silent bit-flip: flips one bit of one byte of *durable* content in a
  // random file. False when no file has durable bytes to corrupt.
  bool corrupt_random_byte();

  [[nodiscard]] base::u64 unflushed_bytes() const;
  [[nodiscard]] base::u64 bytes_lost_on_crash() const { return bytes_lost_; }
  [[nodiscard]] base::u64 crashes() const { return crashes_; }
  [[nodiscard]] std::size_t file_count() const { return files_.size(); }

 private:
  struct PendingWrite {
    base::u64 offset;
    std::vector<base::u8> bytes;
  };

  struct File {
    std::vector<base::u8> durable;      // the platter, as of the last successful fsync
    std::vector<base::u8> visible;      // what pread returns
    std::vector<PendingWrite> pending;  // issue order; fsync clears it
  };

  struct Handle {
    std::string path;
    OpenMode mode;
  };

  // Resolves an open handle to its file. Returns nullptr with *err set for a stale or
  // closed id, for a file unlinked underneath it, or for a write through a read-only
  // handle when `for_write`.
  File* resolve(FileId file, bool for_write, base::ErrorCode* err);

  Random& rng_;
  DiskFaultConfig faults_;

  // Ordered containers, not hashed ones: crash(), corrupt_random_byte(), and
  // list_directory() all iterate these, and hash order would make the run depend on
  // something the seed does not capture (ER-2).
  std::map<std::string, File> files_;
  std::set<std::string> dirs_;
  std::map<FileId, Handle> open_;

  FileId next_id_ = 0;  // never reused: a crash must invalidate ids, not recycle them
  bool powered_ = true;
  base::u64 bytes_lost_ = 0;
  base::u64 crashes_ = 0;
};

}  // namespace io::sim
