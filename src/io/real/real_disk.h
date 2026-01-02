#pragma once

#include <vector>

#include "io/disk.h"

namespace io::real {

// Positional file I/O over pread/pwrite/fsync.
//
// v1 ships this rather than io_uring on purpose (§15.2): io_uring is on the cut list,
// and shipping a correct pwrite path beats shipping a half-finished ring. The
// interface is what week 7 would swap underneath, not the callers.
class RealDisk final : public Disk {
 public:
  RealDisk() = default;
  ~RealDisk() override;

  RealDisk(const RealDisk&) = delete;
  RealDisk& operator=(const RealDisk&) = delete;

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

 private:
  [[nodiscard]] int fd_for(FileId file) const;

  std::vector<int> fds_;  // indexed by FileId; -1 means closed
};

}  // namespace io::real
