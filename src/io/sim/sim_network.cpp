#include "io/sim/sim_network.h"

#include <algorithm>
#include <cstring>
#include <utility>

// Note the qualification on every scheduler type below. Inside `namespace io::sim`,
// the name `sim` resolves to *this* namespace, not to the simulator's — so the
// scheduler has to be spelled `::sim::`. Two namespaces called sim is a papercut worth
// living with: `io/sim/` is the seam's other implementation and `sim/` is the machine
// that drives it, and both names are the right one for their directory.
namespace io::sim {
namespace {

using base::ErrorCode;

}  // namespace

// ---------------------------------------------------------------- Fabric internals

SimNetwork& Fabric::add_node(base::u32 node, std::string name) {
  auto network = std::make_unique<SimNetwork>(*this, node);
  SimNetwork& ref = *network;
  networks_[node] = std::move(network);
  names_[std::move(name)] = node;
  node_endpoints_[node];  // materialize the slot: poll() takes a stable reference to it
  return ref;
}

Fabric::Endpoint* Fabric::endpoint(ConnId id) {
  if (id == kInvalidConn || id >= endpoints_.size()) return nullptr;
  return &endpoints_[static_cast<std::size_t>(id)];
}

ConnId Fabric::new_endpoint(base::u32 node) {
  const auto id = static_cast<ConnId>(endpoints_.size());
  Endpoint fresh;
  fresh.node = node;
  fresh.open = true;
  endpoints_.push_back(std::move(fresh));
  node_endpoints_[node].push_back(id);
  return id;
}

base::Nanos Fabric::draw_latency() {
  return rng_.next_in_range(config_.min_latency, config_.max_latency);
}

bool Fabric::is_cut(base::u32 from, base::u32 to) const {
  return cuts_.find(std::make_pair(from, to)) != cuts_.end();
}

void Fabric::cut(base::u32 from, base::u32 to) {
  if (from == to) return;
  if (!cuts_.insert(std::make_pair(from, to)).second) return;
  scheduler_.record(::sim::EventTag{::sim::EventKind::kPartitionStart, from, to, 0});
  schedule_partition_resets(from, to);
}

void Fabric::heal(base::u32 from, base::u32 to) {
  if (cuts_.erase(std::make_pair(from, to)) == 0) return;
  scheduler_.record(::sim::EventTag{::sim::EventKind::kPartitionEnd, from, to, 0});
}

void Fabric::cut_both(base::u32 a, base::u32 b) {
  cut(a, b);
  cut(b, a);
}

void Fabric::heal_both(base::u32 a, base::u32 b) {
  heal(a, b);
  heal(b, a);
}

void Fabric::heal_all() {
  const std::set<std::pair<base::u32, base::u32>> current = cuts_;
  for (const auto& [from, to] : current) heal(from, to);
}

void Fabric::schedule_partition_resets(base::u32 from, base::u32 to) {
  // A partition does not kill a connection the instant it appears: TCP retransmits,
  // and only gives up after a while. Modelling the delay matters because it is the
  // window in which a node still believes it has a healthy peer — which is exactly
  // where split-brain bugs live.
  scheduler_.schedule_after(
      config_.partition_reset_delay,
      ::sim::EventTag{::sim::EventKind::kReset, from, to, 0}, [this, from, to] {
        if (!is_cut(from, to)) return;  // healed inside the window; the connection lives

        const auto it = node_endpoints_.find(from);
        if (it == node_endpoints_.end()) return;
        const std::vector<ConnId> ids = it->second;
        for (ConnId id : ids) {
          Endpoint* ep = endpoint(id);
          if (ep == nullptr || ep->listener || ep->reset) continue;
          const Endpoint* peer = endpoint(ep->peer);
          if (peer == nullptr || peer->node != to) continue;
          reset_connection(id, true);
        }
      });
}

void Fabric::reset_connection(ConnId id, bool notify_peer) {
  Endpoint* ep = endpoint(id);
  if (ep == nullptr || ep->reset) return;

  ep->reset = true;
  ep->open = false;
  ++connections_reset_;
  scheduler_.record(::sim::EventTag{::sim::EventKind::kReset, ep->node, id, 0});

  const ConnId peer = ep->peer;
  if (notify_peer && peer != kInvalidConn) reset_connection(peer, false);
}

void Fabric::kill_node_connections(base::u32 node) {
  const auto it = node_endpoints_.find(node);
  if (it == node_endpoints_.end()) return;

  const std::vector<ConnId> ids = it->second;
  for (ConnId id : ids) {
    Endpoint* ep = endpoint(id);
    if (ep == nullptr) continue;
    if (ep->listener) {
      listeners_.erase(std::make_pair(node, ep->port));
      ep->open = false;
      ep->backlog.clear();
    } else {
      reset_connection(id, true);
    }
  }

  // Forget the handlers before forgetting the endpoints. The objects those pointers
  // refer to are about to be destroyed with the node, and a simulated crash that left
  // a live pointer behind would be a use-after-free that only fires under fault
  // injection — the worst possible place to find one.
  for (ConnId id : ids) {
    Endpoint* ep = endpoint(id);
    if (ep == nullptr) continue;
    ep->handler = nullptr;
    ep->interest = Interest::kNone;
  }
  it->second.clear();
}

void Fabric::deliver(ConnId from, ConnId to, const std::string& payload) {
  if (Endpoint* src = endpoint(from); src != nullptr) {
    const auto size = static_cast<base::u64>(payload.size());
    src->in_flight -= (src->in_flight < size) ? src->in_flight : size;
  }

  Endpoint* dst = endpoint(to);
  if (dst == nullptr || dst->reset || !dst->open) {
    bytes_dropped_ += payload.size();
    return;
  }
  dst->rx.append(base::Slice(reinterpret_cast<const base::u8*>(payload.data()), payload.size()));
  bytes_delivered_ += payload.size();
}

// ------------------------------------------------------------------ SimNetwork API

base::Result<ConnId> SimNetwork::listen(base::u16 port, int backlog) {
  // The backlog bound is ignored: dropping a pending connection under backlog pressure
  // is a load-shedding behaviour, and nothing in this project reacts to it differently
  // than to a refused connection.
  (void)backlog;

  base::u16 bound = port;
  if (bound == 0) bound = fabric_.next_ephemeral_port_++;

  const auto key = std::make_pair(node_, bound);
  if (fabric_.listeners_.find(key) != fabric_.listeners_.end()) {
    return base::fail(ErrorCode::kInvalidArgument);  // address already in use
  }

  const ConnId id = fabric_.new_endpoint(node_);
  Fabric::Endpoint* ep = fabric_.endpoint(id);
  ep->listener = true;
  ep->port = bound;
  fabric_.listeners_[key] = id;
  return id;
}

base::Result<base::u16> SimNetwork::local_port(ConnId listener) {
  const Fabric::Endpoint* ep = fabric_.endpoint(listener);
  if (ep == nullptr || !ep->listener || ep->node != node_) {
    return base::fail(ErrorCode::kNotFound);
  }
  return ep->port;
}

base::Result<ConnId> SimNetwork::accept(ConnId listener) {
  Fabric::Endpoint* ep = fabric_.endpoint(listener);
  if (ep == nullptr || !ep->listener || ep->node != node_ || !ep->open) {
    return base::fail(ErrorCode::kNotFound);
  }
  if (ep->backlog.empty()) return base::fail(ErrorCode::kWouldBlock);

  const ConnId conn = ep->backlog.front();
  ep->backlog.erase(ep->backlog.begin());
  return conn;
}

base::Result<ConnId> SimNetwork::connect(std::string_view host, base::u16 port) {
  const auto name = fabric_.names_.find(std::string(host));
  if (name == fabric_.names_.end()) return base::fail(ErrorCode::kNotFound);
  const base::u32 peer_node = name->second;

  if (fabric_.is_cut(node_, peer_node) || fabric_.is_cut(peer_node, node_)) {
    // A real non-blocking connect reports this later, through readiness, after a
    // timeout. Collapsing it to an immediate failure drops a state machine no caller
    // would treat differently — a connect that fails is retried the same way whenever
    // the news arrives.
    return base::fail(ErrorCode::kRequestTimedOut);
  }

  const auto listener = fabric_.listeners_.find(std::make_pair(peer_node, port));
  if (listener == fabric_.listeners_.end()) return base::fail(ErrorCode::kNotFound);
  const ConnId listener_id = listener->second;

  const ConnId client = fabric_.new_endpoint(node_);
  const ConnId server = fabric_.new_endpoint(peer_node);
  // Both allocations first, then the pointers: new_endpoint() can reallocate the
  // endpoint vector, and a pointer taken before it would dangle.
  fabric_.endpoint(client)->peer = server;
  fabric_.endpoint(client)->port = port;
  fabric_.endpoint(server)->peer = client;
  fabric_.endpoint(server)->port = port;

  fabric_.scheduler_.record(
      ::sim::EventTag{::sim::EventKind::kConnect, node_, peer_node, client});

  // The listener does not hear about it until a round trip has passed — which is why
  // a client can legally write bytes before the server has accepted.
  Fabric* fabric = &fabric_;
  fabric_.scheduler_.schedule_after(
      fabric_.draw_latency(),
      ::sim::EventTag{::sim::EventKind::kAccept, peer_node, node_, server},
      [fabric, listener_id, server] {
        Fabric::Endpoint* target = fabric->endpoint(listener_id);
        if (target == nullptr || !target->open || !target->listener) {
          fabric->reset_connection(server, true);  // nobody home any more
          return;
        }
        const Fabric::Endpoint* pending = fabric->endpoint(server);
        if (pending == nullptr || pending->reset) return;
        target->backlog.push_back(server);
      });

  return client;
}

base::Result<std::size_t> SimNetwork::read(ConnId conn, base::MutSlice out) {
  Fabric::Endpoint* ep = fabric_.endpoint(conn);
  if (ep == nullptr || ep->node != node_ || ep->listener) {
    return base::fail(ErrorCode::kNotFound);
  }
  if (ep->reset) return base::fail(ErrorCode::kClosed);

  if (!ep->rx.empty()) {
    const std::size_t n = std::min(out.size(), ep->rx.size());
    std::memcpy(out.data(), ep->rx.data(), n);
    ep->rx.consume(n);
    return n;
  }
  // Ordering matters here: a peer that closed after sending must let the reader drain
  // the buffer first, and only then see EOF. Reporting the close early would lose the
  // last response of every request that raced a shutdown.
  if (ep->peer_closed) return std::size_t{0};
  if (!ep->open) return base::fail(ErrorCode::kClosed);
  return base::fail(ErrorCode::kWouldBlock);
}

base::Result<std::size_t> SimNetwork::write(ConnId conn, base::Slice data) {
  Fabric::Endpoint* ep = fabric_.endpoint(conn);
  if (ep == nullptr || ep->node != node_ || ep->listener) {
    return base::fail(ErrorCode::kNotFound);
  }
  if (ep->reset || !ep->open) return base::fail(ErrorCode::kClosed);
  if (data.empty()) return std::size_t{0};

  const auto window = static_cast<base::u64>(fabric_.config_.send_window_bytes);
  if (ep->in_flight >= window) return base::fail(ErrorCode::kWouldBlock);

  const auto room = static_cast<std::size_t>(window - ep->in_flight);
  std::size_t n = std::min(data.size(), room);
  if (n > 1 &&
      fabric_.rng_.next_bool_with_probability(fabric_.config_.partial_write_probability)) {
    n = 1 + static_cast<std::size_t>(fabric_.rng_.next_below(n));
  }

  const ConnId peer_id = ep->peer;
  const Fabric::Endpoint* peer = fabric_.endpoint(peer_id);
  if (peer == nullptr || peer->reset) {
    // Succeeds locally and vanishes, which is what a socket does until the RST arrives.
    fabric_.bytes_dropped_ += n;
    return n;
  }
  const base::u32 peer_node = peer->node;

  if (fabric_.is_cut(node_, peer_node)) {
    fabric_.bytes_dropped_ += n;
    fabric_.scheduler_.record(::sim::EventTag{::sim::EventKind::kDropped, node_, conn, n});
    return n;
  }

  // Never let a later write land before an earlier one. Drawing an independent latency
  // per write and scheduling it blind would reorder bytes inside a connection, which
  // TCP does not do — and the resulting "corrupt frame" bug would be a pure artifact of
  // the simulator, the most expensive kind of false positive there is.
  base::Nanos at = fabric_.scheduler_.now() + fabric_.draw_latency();
  if (at < ep->last_delivery) at = ep->last_delivery;
  ep->last_delivery = at;
  ep->in_flight += n;

  std::string payload(reinterpret_cast<const char*>(data.data()), n);
  Fabric* fabric = &fabric_;
  fabric_.scheduler_.schedule_at(
      at, ::sim::EventTag{::sim::EventKind::kDeliver, peer_node, peer_id, n},
      [fabric, conn, peer_id, payload = std::move(payload)] {
        fabric->deliver(conn, peer_id, payload);
      });

  fabric_.scheduler_.record(::sim::EventTag{::sim::EventKind::kSend, node_, conn, n});
  return n;
}

void SimNetwork::close(ConnId conn) {
  Fabric::Endpoint* ep = fabric_.endpoint(conn);
  if (ep == nullptr || ep->node != node_) return;

  if (ep->listener) {
    fabric_.listeners_.erase(std::make_pair(node_, ep->port));
    const std::vector<ConnId> pending = ep->backlog;
    ep->backlog.clear();
    for (ConnId id : pending) fabric_.reset_connection(id, true);
    ep = fabric_.endpoint(conn);
  } else if (ep->peer != kInvalidConn) {
    if (Fabric::Endpoint* peer = fabric_.endpoint(ep->peer); peer != nullptr) {
      peer->peer_closed = true;
    }
  }

  ep->open = false;
  ep->handler = nullptr;
  ep->interest = Interest::kNone;
}

void SimNetwork::watch(ConnId conn, Interest interest, ConnHandler* handler) {
  Fabric::Endpoint* ep = fabric_.endpoint(conn);
  if (ep == nullptr || ep->node != node_) return;
  ep->interest = interest;
  ep->handler = handler;
}

std::size_t SimNetwork::poll(base::Nanos timeout) {
  (void)timeout;

  const auto it = fabric_.node_endpoints_.find(node_);
  if (it == fabric_.node_endpoints_.end()) return 0;

  // Indexed, not iterated. A handler may call connect(), which appends to this very
  // vector and can reallocate the endpoint store — so the endpoint pointer is re-fetched
  // after every callback and the loop bound is re-read every time round. The reference
  // itself is stable because it lives in a std::map node.
  std::vector<ConnId>& ids = it->second;
  std::size_t dispatched = 0;

  for (std::size_t i = 0; i < ids.size(); ++i) {
    const ConnId id = ids[i];

    Fabric::Endpoint* ep = fabric_.endpoint(id);
    if (ep == nullptr || ep->handler == nullptr || ep->interest == Interest::kNone) continue;

    if (ep->reset && !ep->hangup_delivered) {
      ep->hangup_delivered = true;
      ConnHandler* handler = ep->handler;
      handler->on_hangup(id);
      ++dispatched;
      continue;
    }

    ep = fabric_.endpoint(id);
    if (ep == nullptr || ep->handler == nullptr) continue;
    const bool readable = ep->listener ? !ep->backlog.empty()
                                       : (!ep->rx.empty() || ep->peer_closed);
    if (readable && wants_read(ep->interest)) {
      ConnHandler* handler = ep->handler;
      handler->on_readable(id);
      ++dispatched;
    }

    ep = fabric_.endpoint(id);
    if (ep == nullptr || ep->handler == nullptr) continue;
    const bool writable = ep->open && !ep->listener && !ep->reset &&
                          ep->in_flight < fabric_.config_.send_window_bytes;
    if (writable && wants_write(ep->interest)) {
      ConnHandler* handler = ep->handler;
      handler->on_writable(id);
      ++dispatched;
    }
  }
  return dispatched;
}

}  // namespace io::sim
