#include "server/broker.h"

#include <algorithm>
#include <utility>

#include "raft/codec.h"
#include "raft/state_file.h"

namespace server {
namespace {

using base::Slice;

constexpr const char* kStateFileName = "raft.state";
constexpr std::size_t kEntryScratchBytes = 64u * 1024u;

}  // namespace

Broker::Broker(BrokerConfig config, runtime::EventLoop& loop, io::Disk& disk,
               io::Random& rng, BrokerObserver* observer)
    : config_(std::move(config)),
      loop_(loop),
      disk_(disk),
      rng_(rng),
      observer_(observer) {
  peer_conns_.assign(config_.peers.size(), io::kInvalidConn);
  // Sized once, because the replication path does not allocate (ER-4).
  entry_buf_.resize(kEntryScratchBytes);
}

Broker::~Broker() {
  // Deliberately no fsync and no flush. A crash destroys this object, and a destructor
  // that tidied up would let the simulator quietly lie about power loss — the exact
  // mistake week 3 made once (`docs/retrospective.md` §5).
  if (state_file_ != io::kInvalidFile) disk_.close(state_file_);
}

base::Status Broker::start() {
  storage::Log::Options options;
  options.segment_max_bytes = config_.segment_max_bytes;

  auto opened = storage::Log::open(disk_, config_.data_dir, options);
  if (!opened) return base::fail(opened.error());
  log_ = std::move(opened).value();

  if (observer_ != nullptr) {
    observer_->on_log_recovered(log_->next_offset(), log_->recovery().bytes_truncated);
  }

  // Raft comes up second, and a failure here keeps the broker down on purpose. A node
  // that cannot read its own recorded vote must not serve.
  raft::HardState hard;
  if (auto loaded = load_hard_state(&hard); !loaded) return base::fail(loaded.error());

  raft::Config raft_config;
  raft_config.id = config_.id;
  for (const BrokerConfig::Peer& peer : config_.peers) raft_config.peers.push_back(peer.id);
  raft_config.election_timeout_ticks = config_.election_timeout_ticks;
  raft_config.election_timeout_jitter_ticks = config_.election_timeout_jitter_ticks;
  raft_config.heartbeat_timeout_ticks = config_.heartbeat_timeout_ticks;
  raft_ = std::make_unique<raft::Node>(std::move(raft_config), rng_, hard);

  // Rebuild the leader epoch cache by scanning batch headers (§12.3). Derived, never
  // persisted: a second durable structure is a second thing that can disagree.
  auto epochs = log_->scan_epochs();
  if (!epochs) return base::fail(epochs.error());
  std::vector<raft::Epoch> restored;
  restored.reserve(epochs.value().size());
  for (const storage::EpochBoundary& boundary : epochs.value()) {
    restored.push_back(raft::Epoch{boundary.epoch, boundary.start_offset});
  }
  raft_->restore_log(log_->next_offset(), restored);

  if (observer_ != nullptr) observer_->on_raft_recovered(hard.term, hard.voted_for);

  auto listener = loop_.network().listen(config_.port, 16);
  if (listener) {
    listener_ = listener.value();
    loop_.network().watch(listener_, io::Interest::kRead, this);
  }

  running_ = true;
  schedule_tick();
  return {};
}

void Broker::stop() { running_ = false; }

base::Status Broker::load_hard_state(raft::HardState* out) {
  auto opened =
      disk_.open(config_.data_dir + "/" + kStateFileName, io::OpenMode::kCreate);
  if (!opened) return base::fail(opened.error());
  state_file_ = opened.value();

  auto size = disk_.size(state_file_);
  if (!size) return base::fail(size.error());
  if (size.value() == 0) {
    // No file, or a file whose first write never reached the platter. Either way this
    // node has provably never voted: an unfsynced vote was never announced, because the
    // announcement is what waits on the fsync.
    *out = raft::HardState{};
    state_sequence_ = 0;
    return {};
  }

  base::u8 bytes[raft::kStateFileBytes] = {};
  auto got = disk_.pread(state_file_, base::MutSlice(bytes, sizeof(bytes)), 0);
  if (!got) return base::fail(got.error());

  auto scanned = raft::scan_state_file(Slice(bytes, got.value()));
  if (!scanned) return base::fail(scanned.error());

  *out = scanned.value().hard;
  state_sequence_ = scanned.value().sequence + 1;
  return {};
}

bool Broker::is_leader() const noexcept { return raft_ != nullptr && raft_->is_leader(); }
raft::Term Broker::term() const noexcept { return raft_ != nullptr ? raft_->term() : 0; }
raft::NodeId Broker::leader() const noexcept {
  return raft_ != nullptr ? raft_->leader() : raft::kNoNode;
}
base::u64 Broker::commit_index() const noexcept {
  return raft_ != nullptr ? raft_->commit_index() : 0;
}
base::u64 Broker::log_end() const noexcept {
  return raft_ != nullptr ? raft_->log_end() : 0;
}

void Broker::schedule_tick() {
  if (!running_) return;
  loop_.add_timer_after(config_.tick_interval, [this] { on_tick(); });
}

void Broker::on_tick() {
  if (!running_ || raft_ == nullptr) return;
  ++ticks_;

  const raft::Role role = raft_->role();
  const raft::Term term = raft_->term();
  const raft::NodeId vote = raft_->voted_for();
  raft_->tick();
  note_transition(role, term, vote);
  drive(Slice());

  schedule_tick();
}

base::Result<base::u64> Broker::propose(storage::BatchBuilder& builder) {
  if (!running_ || log_ == nullptr || raft_ == nullptr) {
    return base::fail(base::ErrorCode::kClosed);
  }
  // Only the leader appends. A follower writing locally would manufacture exactly the
  // divergence Raft exists to prevent; a real broker answers NOT_LEADER with a hint.
  if (!raft_->is_leader()) return base::fail(base::ErrorCode::kNotLeader);

  // Every batch carries the term that produced it (§12.3) — what the epoch cache is
  // rebuilt from, and what makes divergence detectable by comparing offsets *and* epochs.
  storage::BatchMeta meta;
  meta.leader_epoch = static_cast<base::u32>(raft_->term());

  auto assigned = log_->append(builder, meta);
  if (!assigned) return base::fail(assigned.error());
  if (observer_ != nullptr) {
    observer_->on_appended(assigned.value(),
                           static_cast<base::u32>(log_->next_offset() - assigned.value()));
  }

  if (auto flushed = log_->fsync(); !flushed) {
    // Appended but not durable, so the leader does not count itself as holding it. The
    // whole durability argument (§13) is that the promise waits for the platter.
    return base::fail(flushed.error());
  }
  if (observer_ != nullptr) observer_->on_fsynced(log_->next_offset());

  // `acks=1`: the producer is told yes here, on one node's disk, before a peer has seen
  // it. Not a bug — a different promise, and the one that loses records when this leader
  // dies before replicating (§13.2).
  if (config_.ack_on_local_append) {
    note_commit(log_->next_offset());
  }

  raft_->log_appended(assigned.value(), log_->next_offset(), raft_->term());
  ++appends_;
  drive(Slice());
  return assigned.value();
}

// **Persist, then send. This function is §13.**
//
// Everything the state machine wants done arrives in one batch, and the order below is
// the durability contract: log orders first, then hard state, then messages. The response
// this broker is about to send says "I have it", and it must be true on the platter
// before it is said.
//
// It is one function on purpose. There is no second code path that sends a Raft message.
void Broker::drive(Slice entry) {
  if (raft_ == nullptr) return;

  raft::Ready ready = raft_->take_ready();
  note_commit(ready.commit_index);
  if (ready.empty()) return;

  if (auto stored = carry_out_log_orders(ready, entry); !stored) {
    // The disk refused, and the node's idea of its own log is now ahead of what is on it.
    // Dropping the messages keeps the lie off the wire but does not repair the broker, so
    // it stops serving until it is restarted and `start()` rebuilds Raft's view from what
    // storage actually holds. Down and honest is the only safe direction.
    running_ = false;
    return;
  }

  if (ready.persist_hard_state) {
    if (auto stored = persist(ready.hard_state); !stored) {
      // Nothing that depends on this state may be said, so the whole batch is dropped —
      // messages included. The cost is one election; peers time out and retry, which is
      // the failure Raft is built to absorb. The node is *not* told the write succeeded,
      // so it still owes one and will try again before it says anything.
      return;
    }
    raft_->hard_state_persisted();
  }

  for (const raft::Message& message : ready.messages) send_raft(message);
}

base::Status Broker::carry_out_log_orders(const raft::Ready& ready, Slice entry) {
  if (log_ == nullptr) return base::fail(base::ErrorCode::kClosed);
  if (ready.truncate_from == raft::kNoIndex && !ready.append_entry) return {};

  // Truncate first, then append: a divergent tail has to be gone before the replacement
  // can land at the same offset.
  if (ready.truncate_from != raft::kNoIndex) {
    if (auto cut = log_->truncate_to(ready.truncate_from); !cut) return cut;
    if (observer_ != nullptr) observer_->on_truncated(ready.truncate_from);
  }

  if (ready.append_entry) {
    if (entry.empty()) return base::fail(base::ErrorCode::kInvalidRequest);
    if (auto wrote = log_->append_replicated(entry); !wrote) return wrote;
    // §13: a follower that acks an AppendEntries it has not fsynced is how an acked write
    // dies in a power cut.
    if (auto flushed = log_->fsync(); !flushed) return flushed;
    if (observer_ != nullptr) observer_->on_replicated(log_->next_offset());
  }
  return {};
}

void Broker::note_commit(base::u64 commit_index) {
  if (commit_index <= last_commit_seen_) return;
  if (observer_ != nullptr) observer_->on_committed(last_commit_seen_, commit_index);
  last_commit_seen_ = commit_index;
}

base::Status Broker::persist(const raft::HardState& state) {
  if (state_file_ == io::kInvalidFile) return base::fail(base::ErrorCode::kClosed);

  base::u8 slot[raft::kStateSlotBytes];
  raft::encode_state_slot(base::MutSlice(slot, sizeof(slot)), state, state_sequence_);

  auto written = disk_.pwrite(state_file_, Slice(slot, sizeof(slot)),
                              raft::state_slot_offset(state_sequence_));
  if (!written) return base::fail(written.error());
  if (written.value() != sizeof(slot)) return base::fail(base::ErrorCode::kIoError);

  // Not optional, not batched, not tunable (§13). The one fsync in the system that never
  // waits for a group commit: the response it gates is a vote.
  if (config_.fsync_hard_state) {
    if (auto flushed = disk_.fsync(state_file_); !flushed) return base::fail(flushed.error());
  }

  ++state_sequence_;
  ++hard_state_writes_;
  if (observer_ != nullptr) {
    observer_->on_hard_state_persisted(state.term, state.voted_for);
  }
  return {};
}

base::Result<Slice> Broker::read_entry(base::u64 index) {
  if (log_ == nullptr) return base::fail(base::ErrorCode::kClosed);

  auto got = log_->read(index, base::MutSlice(entry_buf_.data(), entry_buf_.size()));
  if (!got) return base::fail(got.error());
  if (got.value() == 0) return base::fail(base::ErrorCode::kOffsetOutOfRange);

  const Slice framed(entry_buf_.data(), got.value());
  auto header = storage::decode_header(framed);
  if (!header) return base::fail(base::ErrorCode::kCorruptRecord);
  // read() hands back the batch *containing* the offset. Raft only ever names a batch
  // boundary, so a mismatch means the node's idea of the log and storage's have drifted.
  if (header.value().base_offset != index) {
    return base::fail(base::ErrorCode::kOffsetOutOfRange);
  }
  return framed.subslice(0, header.value().total_bytes());
}

void Broker::note_transition(raft::Role role_before, raft::Term term_before,
                             raft::NodeId vote_before) {
  const raft::Role role = raft_->role();
  const raft::Term term = raft_->term();
  if (observer_ == nullptr) return;

  if (raft_->voted_for() != vote_before && raft_->voted_for() != raft::kNoNode &&
      raft_->voted_for() != config_.id) {
    observer_->on_vote(term, raft_->voted_for());
  }

  if (role == role_before && term == term_before) return;

  if (role == raft::Role::kCandidate) {
    observer_->on_campaign(term);
  } else if (role == raft::Role::kLeader) {
    // Reported the instant a majority is counted, before the heartbeat that announces it.
    // A leader that wins and is destroyed by a crash before anyone hears still won.
    observer_->on_became_leader(term, raft_->log_end());
  } else if (role_before != raft::Role::kFollower) {
    observer_->on_stepped_down(term, raft_->leader());
  }
}

void Broker::send_raft(const raft::Message& message) {
  // Every message leaves over *this* broker's own connection to the target, never as a
  // reply on the connection a request arrived on. That keeps the direction of a link
  // failure meaningful: with an asymmetric cut A→B, A's outgoing stream to B is dead
  // while B's to A still works (§14.1).
  std::size_t index = config_.peers.size();
  for (std::size_t i = 0; i < config_.peers.size(); ++i) {
    if (config_.peers[i].id == message.to) {
      index = i;
      break;
    }
  }
  if (index == config_.peers.size()) return;

  raft::Message outgoing = message;
  Slice entry;
  if (outgoing.carries_entry()) {
    auto found = read_entry(outgoing.entry_index);
    if (!found) return;  // transient read error; the next heartbeat tries again
    entry = found.value();

    auto header = storage::decode_header(entry);
    if (!header) return;
    // Only storage knows how far a batch reaches; the node named the index.
    outgoing.entry_end = header.value().next_offset();
  }

  if (peer_conns_[index] == io::kInvalidConn) {
    auto conn = loop_.network().connect(config_.peers[index].host, config_.peers[index].port);
    if (!conn) return;  // partitioned or not listening; the next tick tries again
    peer_conns_[index] = conn.value();
    streams_[conn.value()].outgoing = true;
    loop_.network().watch(conn.value(), io::Interest::kRead, this);
  }

  std::vector<base::u8> payload(raft::message_bytes(entry.size()));
  raft::encode_message(base::MutSlice(payload.data(), payload.size()), outgoing, entry);
  send(peer_conns_[index],
       wire::FrameHeader{raft::api_key_for(outgoing.type), wire::kApiVersion0,
                         next_correlation_++},
       Slice(payload.data(), payload.size()));
  ++raft_sent_;
}

void Broker::deliver_locally(const raft::Message& message, Slice entry) {
  if (raft_ == nullptr) return;

  const raft::Role role = raft_->role();
  const raft::Term term = raft_->term();
  const raft::NodeId vote = raft_->voted_for();
  raft_->step(message);
  note_transition(role, term, vote);
  drive(entry);
}

void Broker::receive_raft(Slice payload) {
  Slice entry;
  auto decoded = raft::decode_message(payload, &entry);
  if (!decoded) {
    if (observer_ != nullptr) {
      observer_->on_protocol_error("undecodable raft message", payload.size(),
                                   raft::kMessageHeaderBytes);
    }
    return;
  }

  const raft::Message& message = decoded.value();
  if (message.to != config_.id) {
    if (observer_ != nullptr) {
      observer_->on_protocol_error("raft message for another node", message.to, message.from);
    }
    return;
  }

  ++raft_received_;
  deliver_locally(message, entry);
}

void Broker::send(io::ConnId conn, const wire::FrameHeader& header, Slice payload) {
  Stream& stream = streams_[conn];
  wire::encode_frame(stream.out, header, payload);
  flush(conn);
}

void Broker::flush(io::ConnId conn) {
  const auto it = streams_.find(conn);
  if (it == streams_.end()) return;
  Stream& stream = it->second;

  while (!stream.out.empty()) {
    auto written = loop_.network().write(conn, stream.out.slice());
    if (!written) {
      if (written.error() == base::ErrorCode::kWouldBlock) {
        // Backpressure. Park the rest and let readiness say when there is room — spinning
        // on a full socket starves every other connection this loop owns.
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

void Broker::on_writable(io::ConnId conn) { flush(conn); }

void Broker::on_readable(io::ConnId conn) {
  if (conn == listener_) {
    while (true) {
      auto accepted = loop_.network().accept(listener_);
      if (!accepted) return;
      streams_[accepted.value()].outgoing = false;
      loop_.network().watch(accepted.value(), io::Interest::kRead, this);
    }
  }

  // The stream is looked up fresh on every pass rather than held across the loop: a reply
  // can fail to write, and a failed write calls forget(), which erases this connection
  // from streams_ — so any reference taken before that point is dangling by the time the
  // loop comes back around.
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
        if (observer_ != nullptr) observer_->on_protocol_error("undecodable frame", conn, 0);
        forget(conn);
        return;
      }
      if (!ready.value()) break;

      if (!wire::is_broker_api(header.api_key)) {
        if (observer_ != nullptr) {
          observer_->on_protocol_error("non-broker api key on a peer connection",
                                       static_cast<base::u64>(header.api_key), 0);
        }
        forget(conn);
        return;
      }
      receive_raft(payload);

      // The payload view pointed into the read buffer and is dead now. Re-find, because
      // handling may have taken the connection down.
      auto after = streams_.find(conn);
      if (after == streams_.end()) return;
      after->second.decoder.consume_frame(after->second.in);
    }
  }
}

void Broker::on_hangup(io::ConnId conn) { forget(conn); }

void Broker::forget(io::ConnId conn) {
  loop_.network().watch(conn, io::Interest::kNone, nullptr);
  loop_.network().close(conn);
  streams_.erase(conn);
  for (io::ConnId& held : peer_conns_) {
    if (held == conn) held = io::kInvalidConn;  // reconnect on the next tick
  }
}

}  // namespace server
