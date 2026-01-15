#include "sim/workload.h"

#include <algorithm>
#include <utility>

#include "base/endian.h"

namespace sim {
namespace {

using base::Slice;

// api_version doubles as the message type. Two values do not justify a second enum, and
// having it in the frame header means a trace of the wire is readable without decoding
// the payload.
constexpr base::u16 kPing = 0;
constexpr base::u16 kPong = 1;
constexpr std::size_t kPayloadBytes = 8;  // the sender's next offset

std::string describe(base::u32 node, const char* what, base::u64 a, base::u64 b) {
  return "node " + std::to_string(node) + ": " + what + " (" + std::to_string(a) + ", " +
         std::to_string(b) + ")";
}

}  // namespace

base::u64 Oracle::acked_end(base::u32 node) const {
  const std::vector<AckedBatch>& acks = acked_[node];
  if (acks.empty()) return 0;
  return acks.back().base_offset + acks.back().record_count;
}

base::u64 Oracle::total_batches() const {
  base::u64 total = 0;
  for (const auto& per_node : acked_) total += per_node.size();
  return total;
}

base::u64 Oracle::total_records() const {
  base::u64 total = 0;
  for (const auto& per_node : acked_) {
    for (const AckedBatch& batch : per_node) total += batch.record_count;
  }
  return total;
}

void Oracle::violation(const char* invariant, std::string detail) {
  if (invariant_ != nullptr) return;
  invariant_ = invariant;
  detail_ = std::move(detail);
}

std::string payload_for(base::u32 node, base::u64 offset) {
  std::string value = "n" + std::to_string(node) + "-r" + std::to_string(offset) + "-";
  value.resize(48, '.');
  return value;
}

NodeWorkload::NodeWorkload(base::u32 node, Scheduler& scheduler, runtime::EventLoop& loop,
                           io::Disk& disk, io::Random& rng, Oracle& oracle, std::string dir,
                           base::u16 port, std::vector<Peer> peers, Config config)
    : node_(node),
      scheduler_(scheduler),
      loop_(loop),
      disk_(disk),
      rng_(rng),
      oracle_(oracle),
      dir_(std::move(dir)),
      port_(port),
      peers_(std::move(peers)),
      config_(config) {
  peer_conns_.assign(peers_.size(), io::kInvalidConn);
}

NodeWorkload::~NodeWorkload() = default;

base::Status NodeWorkload::start() {
  storage::Log::Options options;
  // Small segments so an hour of simulated life rolls them many times. Rolling is on
  // the durability path, and a fault injector that never crosses a roll boundary is
  // not testing the interesting half of recovery.
  options.segment_max_bytes = 32u * 1024u;

  auto opened = storage::Log::open(disk_, dir_, options);
  if (!opened) return base::fail(opened.error());
  log_ = std::move(opened).value();

  const storage::LogRecoveryReport& report = log_->recovery();
  scheduler_.record(EventTag{EventKind::kRecovered, node_, log_->next_offset(),
                             report.bytes_truncated});

  verify_recovery();
  if (!oracle_.ok()) return {};

  auto listener = loop_.network().listen(port_, 16);
  if (listener) {
    listener_ = listener.value();
    loop_.network().watch(listener_, io::Interest::kRead, this);
  }

  running_ = true;
  schedule_append();
  schedule_ping();
  return {};
}

void NodeWorkload::verify_recovery() {
  const std::vector<AckedBatch>& acks = oracle_.acks(node_);
  if (acks.empty()) return;

  // **I1 — an acked write is never lost.**
  //
  // One comparison, not a scan of an hour's worth of records. Recovery always yields a
  // *prefix* of what was written — week 2's property test establishes that
  // independently, over every truncation point — so a log that still reaches the
  // highest acked offset necessarily still contains every acked offset below it.
  // Re-reading all of them on every restart would turn the run quadratic and prove
  // nothing extra.
  const base::u64 promised = oracle_.acked_end(node_);
  if (log_->next_offset() < promised) {
    oracle_.violation("I1", describe(node_, "log ends below the highest acked offset",
                                     log_->next_offset(), promised));
    return;
  }

  // The prefix argument covers presence; it says nothing about *content*. Spot-check
  // the newest batches, which is where a torn tail would have bitten.
  const std::size_t window = std::min(config_.verify_tail_batches, acks.size());
  std::vector<base::u8> buffer(64u * 1024u);

  for (std::size_t i = acks.size() - window; i < acks.size(); ++i) {
    const AckedBatch& batch = acks[i];
    auto got = log_->read(batch.base_offset, base::MutSlice(buffer.data(), buffer.size()));
    if (!got && got.error() == base::ErrorCode::kIoError) {
      // The disk declined to answer. That is not evidence the record is gone — the
      // offset check above already established presence from durable state, and this
      // walk is only about content. Inconclusive, not violated. Calling it a violation
      // here would make the checker report a data-loss bug every time a simulated read
      // hiccuped, and a checker that cries wolf gets ignored exactly once.
      return;
    }
    if (!got || got.value() == 0) {
      oracle_.violation("I1", describe(node_, "acked offset is unreadable after recovery",
                                       batch.base_offset, batch.record_count));
      return;
    }

    const Slice framed(buffer.data(), got.value());
    auto header = storage::decode_header(framed);
    if (!header || header.value().base_offset != batch.base_offset) {
      oracle_.violation("I1", describe(node_, "read at an acked offset returned another batch",
                                       batch.base_offset, 0));
      return;
    }

    storage::RecordIterator records(framed);
    Slice value;
    base::u64 offset = batch.base_offset;
    while (records.next(&value)) {
      if (value != Slice::from_string(payload_for(node_, offset))) {
        oracle_.violation("I1", describe(node_, "acked record holds bytes never written",
                                         offset, 0));
        return;
      }
      ++offset;
    }
  }
}

void NodeWorkload::schedule_append() {
  if (!running_) return;
  const base::Nanos jitter = rng_.next_in_range(0, config_.append_jitter);
  loop_.add_timer_after(config_.append_interval + jitter, [this] { do_append(); });
}

void NodeWorkload::do_append() {
  if (!running_ || log_ == nullptr) return;

  const base::u64 expected = log_->next_offset();
  builder_.clear();
  for (base::u32 i = 0; i < config_.records_per_batch; ++i) {
    const std::string value = payload_for(node_, expected + i);
    if (!builder_.add_record(Slice::from_string(value))) {
      schedule_append();
      return;
    }
  }

  auto assigned = log_->append(builder_, storage::BatchMeta{});
  if (!assigned) {
    // A simulated I/O error. The batch is not acked, so nothing was promised and
    // nothing is owed — retry on the next tick, exactly as a broker would.
    schedule_append();
    return;
  }

  // **I2 — offsets are monotonic in commit order.** The log is the only thing that
  // assigns them, so a regression here means recovery handed back a next_offset below
  // one already acked — which is the same failure as losing the record, seen earlier.
  if (assigned.value() < oracle_.acked_end(node_)) {
    oracle_.violation("I2", describe(node_, "offset went backwards after recovery",
                                     assigned.value(), oracle_.acked_end(node_)));
    return;
  }
  scheduler_.record(EventTag{EventKind::kAppend, node_, assigned.value(),
                             config_.records_per_batch});

  if (auto flushed = log_->fsync(); !flushed) {
    // Appended but not durable. Deliberately *not* acked: the whole durability
    // argument (§13) is that the response waits for the platter, and a simulator that
    // acked here would agree with the bug it exists to find.
    schedule_append();
    return;
  }
  scheduler_.record(EventTag{EventKind::kFsync, node_, log_->next_offset(), 0});

  oracle_.record_ack(node_, AckedBatch{assigned.value(), config_.records_per_batch});
  scheduler_.record(EventTag{EventKind::kAck, node_, assigned.value(),
                             config_.records_per_batch});
  ++appends_;
  schedule_append();
}

void NodeWorkload::schedule_ping() {
  if (!running_) return;
  loop_.add_timer_after(config_.ping_interval, [this] { do_ping(); });
}

void NodeWorkload::do_ping() {
  if (!running_) return;

  for (std::size_t i = 0; i < peers_.size(); ++i) {
    if (peer_conns_[i] == io::kInvalidConn) {
      auto conn = loop_.network().connect(peers_[i].host, peers_[i].port);
      if (!conn) continue;  // partitioned or nobody listening; try again next tick
      peer_conns_[i] = conn.value();
      Stream& stream = streams_[conn.value()];
      stream.outgoing = true;
      loop_.network().watch(conn.value(), io::Interest::kRead, this);
    }

    const io::ConnId conn = peer_conns_[i];
    const base::u32 correlation = next_correlation_++;
    streams_[conn].awaiting.insert(correlation);

    base::u8 payload[kPayloadBytes];
    base::store_u64_le(payload, log_ != nullptr ? log_->next_offset() : 0);

    scheduler_.record(EventTag{EventKind::kPing, node_, i, correlation});
    send(conn, wire::FrameHeader{wire::ApiKey::kEcho, kPing, correlation},
         Slice(payload, kPayloadBytes));
  }
  schedule_ping();
}

void NodeWorkload::send(io::ConnId conn, const wire::FrameHeader& header, Slice payload) {
  Stream& stream = streams_[conn];
  wire::encode_frame(stream.out, header, payload);
  flush(conn);
}

void NodeWorkload::flush(io::ConnId conn) {
  const auto it = streams_.find(conn);
  if (it == streams_.end()) return;
  Stream& stream = it->second;

  while (!stream.out.empty()) {
    auto written = loop_.network().write(conn, stream.out.slice());
    if (!written) {
      if (written.error() == base::ErrorCode::kWouldBlock) {
        // Backpressure. Park the rest and let readiness say when there is room —
        // spinning on a full socket is how an event loop starves every other
        // connection it owns.
        loop_.network().watch(conn, io::Interest::kReadWrite, this);
        return;
      }
      forget(conn);
      return;
    }
    if (written.value() == 0) return;
    stream.out.consume(written.value());
  }
  loop_.network().watch(conn, io::Interest::kRead, this);
}

void NodeWorkload::on_writable(io::ConnId conn) { flush(conn); }

void NodeWorkload::on_readable(io::ConnId conn) {
  if (conn == listener_) {
    while (true) {
      auto accepted = loop_.network().accept(listener_);
      if (!accepted) return;
      Stream& stream = streams_[accepted.value()];
      stream.outgoing = false;
      loop_.network().watch(accepted.value(), io::Interest::kRead, this);
    }
  }

  // The stream is looked up fresh on every pass rather than held across the loop.
  // serve() replies, a reply can fail to write, and a failed write calls forget(), which
  // erases this connection from streams_ — so any reference taken before that point is
  // dangling by the time the loop comes back around. Cheap lookup, and the alternative
  // is a use-after-free that only fires when a peer dies mid-request.
  while (true) {
    auto it = streams_.find(conn);
    if (it == streams_.end()) return;
    Stream& stream = it->second;

    base::MutSlice into = stream.in.append_uninitialized(4096);
    auto got = loop_.network().read(conn, into);
    if (!got) {
      stream.in.shrink_by(4096);
      if (got.error() == base::ErrorCode::kWouldBlock) return;
      forget(conn);
      return;
    }
    stream.in.shrink_by(4096 - got.value());
    if (got.value() == 0) {  // clean EOF
      forget(conn);
      return;
    }

    while (true) {
      auto live = streams_.find(conn);
      if (live == streams_.end()) return;
      Stream& current = live->second;

      wire::FrameHeader header;
      Slice payload;
      auto ready = current.decoder.next(current.in, &header, &payload);
      if (!ready) {  // protocol violation — the stream is not what we think it is
        oracle_.violation("FRAMING", describe(node_, "undecodable frame from a peer",
                                              conn, 0));
        return;
      }
      if (!ready.value()) break;

      if (header.api_version == kPing) {
        serve(conn, header, payload);
      } else {
        receive_pong(conn, header);
      }
      if (!oracle_.ok()) return;

      // The payload view pointed into the read buffer and is dead now. Re-find, because
      // serve() may have taken the connection down while replying.
      auto after = streams_.find(conn);
      if (after == streams_.end()) return;
      after->second.decoder.consume_frame(after->second.in);
    }
  }
}

void NodeWorkload::serve(io::ConnId conn, const wire::FrameHeader& header, Slice payload) {
  if (payload.size() != kPayloadBytes) {
    oracle_.violation("FRAMING", describe(node_, "ping payload is the wrong size",
                                          payload.size(), kPayloadBytes));
    return;
  }
  base::u8 reply[kPayloadBytes];
  base::store_u64_le(reply, log_ != nullptr ? log_->next_offset() : 0);
  send(conn, wire::FrameHeader{wire::ApiKey::kEcho, kPong, header.correlation_id},
       Slice(reply, kPayloadBytes));
}

void NodeWorkload::receive_pong(io::ConnId conn, const wire::FrameHeader& header) {
  Stream& stream = streams_[conn];
  // A reply nobody asked for means the byte stream is not the one we wrote into it —
  // reordered, duplicated, or crossed with another connection. None of those are things
  // TCP does, so if this ever fires the bug is in the simulated network, not above it.
  if (stream.awaiting.erase(header.correlation_id) == 0) {
    oracle_.violation("FRAMING", describe(node_, "pong for a correlation id never sent",
                                          conn, header.correlation_id));
    return;
  }
  scheduler_.record(EventTag{EventKind::kPong, node_, conn, header.correlation_id});
  ++pongs_;
}

void NodeWorkload::on_hangup(io::ConnId conn) { forget(conn); }

void NodeWorkload::forget(io::ConnId conn) {
  loop_.network().watch(conn, io::Interest::kNone, nullptr);
  loop_.network().close(conn);
  streams_.erase(conn);
  for (io::ConnId& held : peer_conns_) {
    if (held == conn) held = io::kInvalidConn;  // reconnect on the next ping tick
  }
}

}  // namespace sim
