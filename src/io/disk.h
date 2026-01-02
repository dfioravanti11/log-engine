#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"

namespace io {

using FileId = base::u64;
inline constexpr FileId kInvalidFile = ~static_cast<FileId>(0);

enum class OpenMode : base::u8 {
  kRead,        // must exist
  kReadWrite,   // must exist
  kCreate,      // create if absent, open read-write, do not truncate
};

// Positional file I/O behind the seam.
//
// fsync() is a separate call from pwrite() on purpose — that separation is the whole
// durability argument (§13). A write that reached the page cache and a write that
// reached the platter are different events with different guarantees, and the
// simulator's unflushed-write-loss fault only makes sense if the code can tell them
// apart. Any design where "write" implies "durable" has already lost the argument.
class Disk {
 public:
  virtual ~Disk() = default;

  virtual base::Result<FileId> open(std::string_view path, OpenMode mode) = 0;
  virtual void close(FileId file) = 0;

  virtual base::Result<std::size_t> pwrite(FileId file, base::Slice data, base::u64 offset) = 0;
  virtual base::Result<std::size_t> pread(FileId file, base::MutSlice out, base::u64 offset) = 0;

  // Returns only once the data is durable. This is the call the simulator delays,
  // reorders, and drops writes behind.
  virtual base::Status fsync(FileId file) = 0;

  virtual base::Status truncate(FileId file, base::u64 size) = 0;
  virtual base::Result<base::u64> size(FileId file) = 0;

  virtual base::Status remove(std::string_view path) = 0;
  virtual base::Status make_directories(std::string_view path) = 0;

  // Entry names (not paths), `.` and `..` excluded, **sorted**.
  //
  // Sorted because readdir() order is filesystem-dependent, and a log that recovers
  // its segments in whatever order the kernel felt like handing them over is a log
  // whose recovery is not reproducible (ER-2). Sorting here costs nothing and means
  // storage/ never has to think about it.
  virtual base::Result<std::vector<std::string>> list_directory(std::string_view path) = 0;
};

}  // namespace io
