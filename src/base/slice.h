#pragma once

#include <cassert>
#include <cstring>
#include <string_view>

#include "base/types.h"

namespace base {

// Non-owning view over bytes. Lifetime is the caller's problem — Slices are passed
// down the stack, never stored.
class Slice {
 public:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  constexpr Slice() = default;
  constexpr Slice(const u8* data, std::size_t size) : data_(data), size_(size) {}

  static Slice from_string(std::string_view s) {
    return Slice(reinterpret_cast<const u8*>(s.data()), s.size());
  }

  [[nodiscard]] constexpr const u8* data() const noexcept { return data_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  [[nodiscard]] const u8* begin() const noexcept { return data_; }
  [[nodiscard]] const u8* end() const noexcept { return data_ + size_; }

  u8 operator[](std::size_t i) const {
    assert(i < size_);
    return data_[i];
  }

  [[nodiscard]] Slice subslice(std::size_t offset, std::size_t count = npos) const {
    assert(offset <= size_);
    const std::size_t avail = size_ - offset;
    return Slice(data_ + offset, count == npos ? avail : (count < avail ? count : avail));
  }

  [[nodiscard]] std::string_view as_string_view() const {
    return {reinterpret_cast<const char*>(data_), size_};
  }

  friend bool operator==(const Slice& a, const Slice& b) {
    if (a.size_ != b.size_) return false;
    if (a.size_ == 0) return true;
    return std::memcmp(a.data_, b.data_, a.size_) == 0;
  }
  friend bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }

 private:
  const u8* data_ = nullptr;
  std::size_t size_ = 0;
};

// Writable counterpart, used for read destinations (io::Network::read, io::Disk::pread).
class MutSlice {
 public:
  constexpr MutSlice() = default;
  constexpr MutSlice(u8* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] constexpr u8* data() const noexcept { return data_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }

  [[nodiscard]] MutSlice subslice(std::size_t offset, std::size_t count = Slice::npos) const {
    assert(offset <= size_);
    const std::size_t avail = size_ - offset;
    return MutSlice(data_ + offset,
                    count == Slice::npos ? avail : (count < avail ? count : avail));
  }

  [[nodiscard]] Slice as_slice() const noexcept { return Slice(data_, size_); }

 private:
  u8* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace base
