#include "io/real/real_disk.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <string>

namespace io::real {
namespace {

base::ErrorCode errno_to_code() {
  switch (errno) {
    case ENOENT:
      return base::ErrorCode::kNotFound;
    case EINVAL:
      return base::ErrorCode::kInvalidArgument;
    default:
      return base::ErrorCode::kIoError;
  }
}

}  // namespace

RealDisk::~RealDisk() {
  for (int fd : fds_) {
    if (fd >= 0) ::close(fd);
  }
}

int RealDisk::fd_for(FileId file) const {
  if (file >= fds_.size()) return -1;
  return fds_[static_cast<std::size_t>(file)];
}

base::Result<FileId> RealDisk::open(std::string_view path, OpenMode mode) {
  int flags = 0;
  switch (mode) {
    case OpenMode::kRead: flags = O_RDONLY; break;
    case OpenMode::kReadWrite: flags = O_RDWR; break;
    case OpenMode::kCreate: flags = O_RDWR | O_CREAT; break;
  }

  const std::string path_str(path);
  const int fd = ::open(path_str.c_str(), flags, 0644);
  if (fd < 0) return base::fail(errno_to_code());

  const auto id = static_cast<FileId>(fds_.size());
  fds_.push_back(fd);
  return id;
}

void RealDisk::close(FileId file) {
  const int fd = fd_for(file);
  if (fd < 0) return;
  ::close(fd);
  fds_[static_cast<std::size_t>(file)] = -1;
}

base::Result<std::size_t> RealDisk::pwrite(FileId file, base::Slice data, base::u64 offset) {
  const int fd = fd_for(file);
  if (fd < 0) return base::fail(base::ErrorCode::kNotFound);

  // Short writes are legal and must be handled, not asserted away.
  std::size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::pwrite(fd, data.data() + written, data.size() - written,
                               static_cast<off_t>(offset + written));
    if (n < 0) {
      if (errno == EINTR) continue;
      return base::fail(errno_to_code());
    }
    if (n == 0) break;
    written += static_cast<std::size_t>(n);
  }
  return written;
}

base::Result<std::size_t> RealDisk::pread(FileId file, base::MutSlice out, base::u64 offset) {
  const int fd = fd_for(file);
  if (fd < 0) return base::fail(base::ErrorCode::kNotFound);

  std::size_t got = 0;
  while (got < out.size()) {
    const ssize_t n =
        ::pread(fd, out.data() + got, out.size() - got, static_cast<off_t>(offset + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      return base::fail(errno_to_code());
    }
    if (n == 0) break;  // EOF: a short read is the caller's signal, not an error
    got += static_cast<std::size_t>(n);
  }
  return got;
}

base::Status RealDisk::fsync(FileId file) {
  const int fd = fd_for(file);
  if (fd < 0) return base::fail(base::ErrorCode::kNotFound);

#if defined(__APPLE__)
  // On macOS, fsync() only pushes to the drive's cache; F_FULLFSYNC is what actually
  // reaches the platter. Benchmarks that skip this are measuring the wrong thing, and
  // a durability claim made on plain fsync() here would simply be false.
  if (::fcntl(fd, F_FULLFSYNC) == 0) return {};
  if (errno != ENOTSUP && errno != EINVAL) return base::fail(errno_to_code());
#endif
  if (::fsync(fd) != 0) return base::fail(errno_to_code());
  return {};
}

base::Status RealDisk::truncate(FileId file, base::u64 size) {
  const int fd = fd_for(file);
  if (fd < 0) return base::fail(base::ErrorCode::kNotFound);
  if (::ftruncate(fd, static_cast<off_t>(size)) != 0) return base::fail(errno_to_code());
  return {};
}

base::Result<base::u64> RealDisk::size(FileId file) {
  const int fd = fd_for(file);
  if (fd < 0) return base::fail(base::ErrorCode::kNotFound);
  struct stat st {};
  if (::fstat(fd, &st) != 0) return base::fail(errno_to_code());
  return static_cast<base::u64>(st.st_size);
}

base::Status RealDisk::remove(std::string_view path) {
  const std::string path_str(path);
  if (::unlink(path_str.c_str()) != 0) return base::fail(errno_to_code());
  return {};
}

base::Status RealDisk::make_directories(std::string_view path) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path), ec);
  if (ec) return base::fail(base::ErrorCode::kIoError);
  return {};
}

base::Result<std::vector<std::string>> RealDisk::list_directory(std::string_view path) {
  std::error_code ec;
  std::filesystem::directory_iterator it(std::filesystem::path(path), ec);
  if (ec) {
    return base::fail(ec == std::errc::no_such_file_or_directory ? base::ErrorCode::kNotFound
                                                                : base::ErrorCode::kIoError);
  }

  std::vector<std::string> names;
  for (const auto& entry : it) {
    names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}

}  // namespace io::real
