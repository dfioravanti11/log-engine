#pragma once

#include <string_view>

#include "base/result.h"
#include "base/slice.h"
#include "base/types.h"

namespace io {

// Opaque connection handle. The real implementation happens to use the fd value;
// the simulator hands out synthetic ids. Nothing above the seam may assume either.
using ConnId = base::u64;
inline constexpr ConnId kInvalidConn = ~static_cast<ConnId>(0);

enum class Interest : base::u8 {
  kNone = 0,
  kRead = 1,
  kWrite = 2,
  kReadWrite = 3,
};

constexpr bool wants_read(Interest i) noexcept {
  return (static_cast<base::u8>(i) & static_cast<base::u8>(Interest::kRead)) != 0;
}
constexpr bool wants_write(Interest i) noexcept {
  return (static_cast<base::u8>(i) & static_cast<base::u8>(Interest::kWrite)) != 0;
}

class ConnHandler {
 public:
  virtual ~ConnHandler() = default;

  // Readable also fires for a listening socket with a pending accept().
  virtual void on_readable(ConnId conn) = 0;
  virtual void on_writable(ConnId conn) = 0;

  // Transport-level failure. Ordinary peer close surfaces as read() == 0 bytes,
  // not as a hangup, so the handler drains what is already buffered first.
  virtual void on_hangup(ConnId conn) = 0;
};

// Readiness-based, non-blocking network. poll() is the only call that waits, which
// is what lets the simulator replace the whole thing with a message queue and
// virtual time while everything above stays byte-identical.
class Network {
 public:
  virtual ~Network() = default;

  // port == 0 binds an ephemeral port; recover it with local_port().
  virtual base::Result<ConnId> listen(base::u16 port, int backlog) = 0;
  virtual base::Result<base::u16> local_port(ConnId listener) = 0;

  // kWouldBlock when no connection is pending.
  virtual base::Result<ConnId> accept(ConnId listener) = 0;

  virtual base::Result<ConnId> connect(std::string_view host, base::u16 port) = 0;

  // 0 bytes read means the peer closed cleanly. kWouldBlock means try again later.
  virtual base::Result<std::size_t> read(ConnId conn, base::MutSlice out) = 0;
  virtual base::Result<std::size_t> write(ConnId conn, base::Slice data) = 0;

  virtual void close(ConnId conn) = 0;

  // handler may be null only when interest is kNone.
  virtual void watch(ConnId conn, Interest interest, ConnHandler* handler) = 0;

  // Waits up to `timeout` (base::kNoTimeout blocks indefinitely) and dispatches
  // readiness to handlers. Returns the number of events dispatched.
  virtual std::size_t poll(base::Nanos timeout) = 0;
};

}  // namespace io
