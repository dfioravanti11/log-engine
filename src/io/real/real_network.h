#pragma once

#include <vector>

#include "io/network.h"

namespace io::real {

// Readiness-based TCP over kqueue (macOS/BSD) or epoll (Linux). Which backend is
// compiled in is invisible above the seam — and so is the fact that a backend exists
// at all, which is what lets io::sim replace the whole class in week 3.
//
// Single-threaded by contract: one RealNetwork per event loop, per core. It is not
// safe to call from two threads, and it never needs to be — cross-core work goes
// through the MPSC queue in runtime/, not through shared network state.
class RealNetwork final : public Network {
 public:
  // `bind_all_interfaces` is the difference between a cluster on one machine and a
  // cluster on three. It is a property of the deployment, not of a call, so it lives
  // here rather than widening `Network::listen()` — which would push a detail of the
  // real socket API through the seam and into io/sim/, where it means nothing.
  //
  // Defaults to loopback. A benchmark VM opts in with `logengine --bind-all`, so the
  // laptop case can never accidentally expose a broker to the local network.
  explicit RealNetwork(bool bind_all_interfaces = false);
  ~RealNetwork() override;

  RealNetwork(const RealNetwork&) = delete;
  RealNetwork& operator=(const RealNetwork&) = delete;

  base::Result<ConnId> listen(base::u16 port, int backlog) override;
  base::Result<base::u16> local_port(ConnId listener) override;
  base::Result<ConnId> accept(ConnId listener) override;
  base::Result<ConnId> connect(std::string_view host, base::u16 port) override;
  base::Result<std::size_t> read(ConnId conn, base::MutSlice out) override;
  base::Result<std::size_t> write(ConnId conn, base::Slice data) override;
  void close(ConnId conn) override;
  void watch(ConnId conn, Interest interest, ConnHandler* handler) override;
  std::size_t poll(base::Nanos timeout) override;

  [[nodiscard]] std::size_t open_connection_count() const;

 private:
  struct Entry {
    bool active = false;
    Interest interest = Interest::kNone;
    ConnHandler* handler = nullptr;
  };

  Entry* entry_for(ConnId conn);
  void ensure_slot(int fd);
  void apply_interest(int fd, Interest before, Interest after);
  base::Status register_fd(int fd);

  int poll_fd_ = -1;
  bool bind_all_ = false;
  std::vector<Entry> entries_;  // indexed by fd; fds are small and dense

  // fds closed while dispatching the current poll batch. An event captured before
  // the close must not be delivered to whatever inherits that fd number afterwards.
  std::vector<bool> closed_during_dispatch_;
};

}  // namespace io::real
