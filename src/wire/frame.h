#pragma once

#include "base/buffer.h"
#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"
#include "wire/api.h"

namespace wire {

// Frame layout (§16.3), little-endian:
//
//   u32 length | u16 api_key | u16 api_version | u32 correlation_id | payload
//
// `length` counts everything after itself: header + payload. correlation_id is what
// makes pipelining possible — several requests in flight on one connection, with
// responses free to come back out of order.
inline constexpr std::size_t kLengthFieldBytes = 4;
inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kFramePrefixBytes = kLengthFieldBytes + kHeaderBytes;

// 16 MiB. The limit exists to be enforced *before* allocating anything, because a
// four-byte length field an attacker controls is the cheapest OOM in the world and
// is the first thing the fuzzer will reach for.
inline constexpr base::u32 kDefaultMaxFrameBytes = 16u * 1024u * 1024u;

struct FrameHeader {
  ApiKey api_key = ApiKey::kEcho;
  base::u16 api_version = kApiVersion0;
  base::u32 correlation_id = 0;
};

// Appends a complete frame to `out`.
void encode_frame(base::Buffer& out, const FrameHeader& header, base::Slice payload);

// Incremental decoder: feed it a read buffer, take out whole frames.
//
// The payload handed back is a view into `buf` and stays valid only until the next
// call that touches that buffer. Nothing needs to copy it — the echo path writes it
// straight back out — but nothing may hold onto it either.
class FrameDecoder {
 public:
  explicit FrameDecoder(base::u32 max_frame_bytes = kDefaultMaxFrameBytes)
      : max_frame_bytes_(max_frame_bytes) {}

  // Peeks the next complete frame. Does NOT consume it: the payload view points
  // into `buf`, so consuming here would hand back a view that dangles on the very
  // next append. Call consume_frame() once done with the payload.
  //
  // true  → a frame is available in *header / *payload
  // false → need more bytes; `buf` is untouched
  // error → protocol violation; close the connection, do not retry
  base::Result<bool> next(base::Buffer& buf, FrameHeader* header, base::Slice* payload);

  // Drops the frame most recently returned by next(). Invalidates that payload view.
  void consume_frame(base::Buffer& buf);

  [[nodiscard]] base::u32 max_frame_bytes() const noexcept { return max_frame_bytes_; }

  // How many more bytes the current partial frame needs, or 0 if unknown yet.
  // Lets the reader size its next recv() instead of guessing.
  [[nodiscard]] std::size_t bytes_needed(const base::Buffer& buf) const;

 private:
  base::u32 max_frame_bytes_;
};

}  // namespace wire
