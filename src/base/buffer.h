#pragma once

#include <cassert>
#include <cstring>
#include <vector>

#include "base/endian.h"
#include "base/slice.h"
#include "base/types.h"

namespace base {

// Growable byte buffer that doubles as a stream reader.
//
// Bytes are appended at the back and consumed from the front, so one Buffer serves
// as a socket read buffer (append what the kernel gave us, consume whole frames) and
// as a write buffer (append an encoded frame, consume what the kernel accepted).
//
// The consumed prefix is not freed immediately; it is reclaimed by compaction once
// it exceeds half the allocation, which keeps steady-state framing at amortized O(1)
// instead of memmove-per-frame.
//
// Week 7 replaces this on the hot path with a per-core pool of fixed-size
// page-aligned buffers (ER-4). This type stays for setup and test paths.
class Buffer {
 public:
  Buffer() = default;
  explicit Buffer(std::size_t capacity) { bytes_.reserve(capacity); }

  [[nodiscard]] const u8* data() const noexcept { return bytes_.data() + read_pos_; }
  [[nodiscard]] u8* mutable_data() noexcept { return bytes_.data() + read_pos_; }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size() - read_pos_; }
  [[nodiscard]] bool empty() const noexcept { return size() == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return bytes_.capacity(); }
  [[nodiscard]] Slice slice() const noexcept { return Slice(data(), size()); }

  void clear() noexcept {
    bytes_.clear();
    read_pos_ = 0;
  }

  void reserve(std::size_t n) { bytes_.reserve(n); }

  // Drop n bytes from the front. Invalidates every Slice previously handed out.
  void consume(std::size_t n) {
    assert(n <= size());
    read_pos_ += n;
    if (read_pos_ == bytes_.size()) {
      clear();
    } else if (read_pos_ * 2 >= bytes_.size()) {
      compact();
    }
  }

  void append(Slice s) {
    if (s.empty()) return;
    bytes_.insert(bytes_.end(), s.begin(), s.end());
  }
  void append(const void* p, std::size_t n) {
    append(Slice(static_cast<const u8*>(p), n));
  }

  void append_u8(u8 v) { bytes_.push_back(v); }
  void append_u16_le(u16 v) { append_fixed<2>([&](u8* p) { store_u16_le(p, v); }); }
  void append_u32_le(u32 v) { append_fixed<4>([&](u8* p) { store_u32_le(p, v); }); }
  void append_u64_le(u64 v) { append_fixed<8>([&](u8* p) { store_u64_le(p, v); }); }
  void append_i64_le(i64 v) { append_fixed<8>([&](u8* p) { store_i64_le(p, v); }); }

  // Grow by n bytes and hand back a writable view of them — used to read straight
  // from a socket into the buffer without a bounce copy.
  [[nodiscard]] MutSlice append_uninitialized(std::size_t n) {
    const std::size_t old = bytes_.size();
    bytes_.resize(old + n);
    return MutSlice(bytes_.data() + old, n);
  }

  // Undo part of an append_uninitialized() that the reader did not fill.
  void shrink_by(std::size_t n) {
    assert(n <= size());
    bytes_.resize(bytes_.size() - n);
  }

 private:
  template <std::size_t N, class Store>
  void append_fixed(Store store) {
    const std::size_t old = bytes_.size();
    bytes_.resize(old + N);
    store(bytes_.data() + old);
  }

  void compact() {
    if (read_pos_ == 0) return;
    const std::size_t remaining = bytes_.size() - read_pos_;
    if (remaining > 0) {
      std::memmove(bytes_.data(), bytes_.data() + read_pos_, remaining);
    }
    bytes_.resize(remaining);
    read_pos_ = 0;
  }

  std::vector<u8> bytes_;
  std::size_t read_pos_ = 0;
};

}  // namespace base
