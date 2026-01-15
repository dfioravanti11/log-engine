#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/buffer.h"
#include "io/network.h"
#include "io/random.h"
#include "sim/scheduler.h"

namespace io::sim {

class Fabric;

// Faults the wires can inject (§14.1), with one deliberate omission.
//
// The fault menu in the spec — drop, reorder, duplicate — is written for message
// passing. This interface is a **byte stream**, and over a byte stream those three are
// not faults, they are fiction: TCP does not hand a reader byte 40 before byte 39, and
// it does not silently lose byte 41 and deliver byte 42. Simulating that would
// manufacture failures no production system can produce, and every hour spent chasing
// one would be an hour spent on a bug that does not exist.
//
// What actually goes wrong on a stream is here instead: latency, backpressure, partial
// writes, partitions, and connections that die. Message-level drop and duplicate
// reappear one layer up, where they belong — a partitioned link kills the connection,
// the RPC never gets its reply, and the caller retries into a new one. That is how a
// message gets lost in reality.
struct NetworkFaultConfig {
  base::Nanos min_latency = base::micros(50);
  base::Nanos max_latency = base::millis(2);

  // How often a write is accepted only in part. Every caller claims to handle a short
  // write; this is what finds out.
  double partial_write_probability = 0.10;

  // A partition does not sever a live connection instantly — TCP retransmits first and
  // gives up later. This is how long a cut link takes to become a reset.
  base::Nanos partition_reset_delay = base::millis(50);

  // Bytes in flight before write() starts returning kWouldBlock. Real backpressure,
  // because a system that has never seen a full socket buffer has never been tested.
  base::u32 send_window_bytes = 64u * 1024u;
};

// One node's view of the network. Implements io::Network exactly, so the code above
// the seam cannot tell it from io::real::RealNetwork.
class SimNetwork final : public Network {
 public:
  SimNetwork(Fabric& fabric, base::u32 node) : fabric_(fabric), node_(node) {}

  base::Result<ConnId> listen(base::u16 port, int backlog) override;
  base::Result<base::u16> local_port(ConnId listener) override;
  base::Result<ConnId> accept(ConnId listener) override;
  base::Result<ConnId> connect(std::string_view host, base::u16 port) override;
  base::Result<std::size_t> read(ConnId conn, base::MutSlice out) override;
  base::Result<std::size_t> write(ConnId conn, base::Slice data) override;
  void close(ConnId conn) override;
  void watch(ConnId conn, Interest interest, ConnHandler* handler) override;

  // Dispatches readiness for this node's connections and returns immediately.
  //
  // It never blocks, which is the one place the simulated network visibly differs from
  // the real one — and it has to be. Blocking here would mean this node deciding how
  // far the clock moves, while the next thing to happen in the cluster may belong to a
  // different node entirely. Waiting is the simulation's job, not a node's, so the
  // `timeout` argument is accepted and ignored.
  std::size_t poll(base::Nanos timeout) override;

 private:
  Fabric& fabric_;
  base::u32 node_;
};

// The wires: every endpoint in the cluster, who can currently reach whom, and the
// scheduled deliveries in flight. One per simulation.
class Fabric {
 public:
  Fabric(::sim::Scheduler& scheduler, Random& rng, NetworkFaultConfig config)
      : scheduler_(scheduler), rng_(rng), config_(config) {}

  Fabric(const Fabric&) = delete;
  Fabric& operator=(const Fabric&) = delete;

  // `name` is what connect() resolves. Nodes are addressed by hostname rather than by
  // index so that server/ and client/ code can carry an ordinary config file into the
  // simulator without a second address type.
  SimNetwork& add_node(base::u32 node, std::string name);
  [[nodiscard]] SimNetwork& network_for(base::u32 node) { return *networks_.at(node); }

  // One direction at a time, because asymmetric partitions are the interesting ones:
  // A can reach B while B cannot reach A is what livelocks a naive election loop
  // (§14.1), and it cannot be expressed by a fault model that only cuts links in pairs.
  void cut(base::u32 from, base::u32 to);
  void heal(base::u32 from, base::u32 to);
  void cut_both(base::u32 a, base::u32 b);
  void heal_both(base::u32 a, base::u32 b);
  void heal_all();
  [[nodiscard]] bool is_cut(base::u32 from, base::u32 to) const;

  // Everything the node had open dies with it.
  void kill_node_connections(base::u32 node);

  [[nodiscard]] base::u64 bytes_delivered() const noexcept { return bytes_delivered_; }
  [[nodiscard]] base::u64 bytes_dropped() const noexcept { return bytes_dropped_; }
  [[nodiscard]] base::u64 connections_reset() const noexcept { return connections_reset_; }

 private:
  friend class SimNetwork;

  struct Endpoint {
    base::u32 node = 0;
    bool listener = false;
    bool open = false;
    bool reset = false;             // peer or partition killed it
    bool hangup_delivered = false;  // on_hangup fires once, not on every poll
    bool peer_closed = false;       // clean EOF once rx drains
    base::u16 port = 0;
    ConnId peer = kInvalidConn;

    base::Buffer rx;
    base::u64 in_flight = 0;      // written but not yet delivered — the send window
    base::Nanos last_delivery = 0;  // never let a later write land before an earlier one

    Interest interest = Interest::kNone;
    ConnHandler* handler = nullptr;
    std::vector<ConnId> backlog;  // listeners only
  };

  Endpoint* endpoint(ConnId id);
  ConnId new_endpoint(base::u32 node);
  void deliver(ConnId from, ConnId to, const std::string& payload);
  void reset_connection(ConnId id, bool notify_peer);
  void schedule_partition_resets(base::u32 from, base::u32 to);
  base::Nanos draw_latency();

  ::sim::Scheduler& scheduler_;
  Random& rng_;
  NetworkFaultConfig config_;

  std::vector<Endpoint> endpoints_;
  std::map<base::u32, std::unique_ptr<SimNetwork>> networks_;
  std::map<base::u32, std::vector<ConnId>> node_endpoints_;
  std::map<std::string, base::u32> names_;
  std::map<std::pair<base::u32, base::u16>, ConnId> listeners_;
  std::set<std::pair<base::u32, base::u32>> cuts_;

  base::u16 next_ephemeral_port_ = 40000;
  base::u64 bytes_delivered_ = 0;
  base::u64 bytes_dropped_ = 0;
  base::u64 connections_reset_ = 0;
};

}  // namespace io::sim
