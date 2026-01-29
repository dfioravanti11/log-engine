#include "sim/workload.h"

#include <algorithm>
#include <utility>


namespace sim {
namespace {

using base::Slice;

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

void Oracle::record_commit(base::u64 end) {
  if (end <= committed_end_) return;
  committed_end_ = end;
  ++commits_;
}

void Oracle::note_leader_present(base::Nanos at) {
  if (leader_present_) return;
  const base::Nanos gap = at - leaderless_since_;
  if (gap > longest_gap_) longest_gap_ = gap;
  // Only a stretch during which a majority was continuously alive is a *failover*. The
  // rest are the cluster correctly declining to elect anybody, and averaging those in
  // measures the fault injector's restart delay rather than the algorithm.
  if (gap_had_quorum_) gaps_.push_back(gap);
  leader_present_ = true;
}

void Oracle::note_leader_absent(base::Nanos at) {
  if (!leader_present_) return;
  leader_present_ = false;
  leaderless_since_ = at;
  gap_had_quorum_ = true;
}

void Oracle::check_leader_completeness(base::u32 node, base::u64 log_end) {
  if (log_end >= committed_end_) return;
  violation("I1", describe(node, "won an election without the committed prefix", log_end,
                           committed_end_));
}

void Oracle::record_leader(raft::Term term, raft::NodeId node) {
  observe_term(term);
  const auto [it, inserted] = leader_by_term_.emplace(term, node);
  if (inserted) {
    ++elections_;
    return;
  }
  if (it->second == node) return;  // the same node re-observed, which is not an election

  // **I6 — at most one leader per term.** Two nodes each counted a majority of the same
  // cluster in the same term, which is arithmetically impossible unless somebody voted
  // twice — so the cause is upstream: a lost vote across a crash, an fsync that was
  // skipped, or a response sent before the write landed (§13).
  violation("I6", "term " + std::to_string(term) + " has two leaders: node " +
                      std::to_string(it->second) + " and node " + std::to_string(node));
}

std::string payload_for(base::u64 offset) {
  // Keyed on the offset alone, not the node. With replication there is one log and every
  // node must hold identical bytes at a given offset — a payload that varied by node
  // would make "these two nodes agree" unaskable.
  std::string value = "r" + std::to_string(offset) + "-";
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
      config_(config) {}

NodeWorkload::~NodeWorkload() = default;

base::Status NodeWorkload::start() {
  server::BrokerConfig broker_config;
  broker_config.id = node_;
  broker_config.data_dir = dir_;
  broker_config.port = port_;
  broker_config.peers = peers_;
  broker_config.tick_interval = config_.raft_tick;
  broker_config.election_timeout_ticks = config_.election_timeout_ticks;
  broker_config.election_timeout_jitter_ticks = config_.election_timeout_jitter_ticks;
  broker_config.heartbeat_timeout_ticks = config_.heartbeat_timeout_ticks;
  broker_config.fsync_hard_state = config_.fsync_hard_state;
  broker_config.ack_on_local_append = config_.ack_on_local_append;
  // Small segments so an hour of simulated life rolls them many times. Rolling is on the
  // durability path, and a fault injector that never crosses a roll boundary is not
  // testing the interesting half of recovery.
  broker_config.segment_max_bytes = 32u * 1024u;

  broker_ = std::make_unique<server::Broker>(std::move(broker_config), loop_, disk_, rng_,
                                             this);
  if (auto started = broker_->start(); !started) {
    broker_.reset();
    return base::fail(started.error());
  }
  if (!oracle_.ok()) return {};

  running_ = true;
  schedule_append();
  return {};
}

base::u64 NodeWorkload::appends() const noexcept {
  return broker_ != nullptr ? broker_->appends() : 0;
}
base::u64 NodeWorkload::raft_messages_sent() const noexcept {
  return broker_ != nullptr ? broker_->raft_messages_sent() : 0;
}
base::u64 NodeWorkload::hard_state_writes() const noexcept {
  return broker_ != nullptr ? broker_->hard_state_writes() : 0;
}
base::u64 NodeWorkload::ticks() const noexcept {
  return broker_ != nullptr ? broker_->ticks() : 0;
}
bool NodeWorkload::is_leader() const noexcept {
  return broker_ != nullptr && broker_->is_leader();
}

// ---- The load generator ----

void NodeWorkload::schedule_append() {
  if (!running_) return;
  const base::Nanos jitter = rng_.next_in_range(0, config_.append_jitter);
  loop_.add_timer_after(config_.append_interval + jitter, [this] { do_append(); });
}

void NodeWorkload::do_append() {
  if (!running_ || broker_ == nullptr) return;

  storage::Log* log = broker_->log();
  if (log == nullptr) return;

  const base::u64 expected = log->next_offset();
  builder_.clear();
  for (base::u32 i = 0; i < config_.records_per_batch; ++i) {
    const std::string value = payload_for(expected + i);
    if (!builder_.add_record(Slice::from_string(value))) {
      schedule_append();
      return;
    }
  }

  // Sampled *before* the append, because under `acks=1` the append commits itself: the
  // broker records the promise inside propose(), so reading the oracle afterwards would
  // compare this offset against a commit index it had just advanced, and every single
  // append would look like a regression. (It did, on the first run after the driver moved
  // into `server/` — the checker caught the refactor, which is the checker working.)
  const base::u64 committed_before = oracle_.committed_end();

  auto assigned = broker_->propose(builder_);
  if (!assigned) {
    // Not the leader, or the disk refused. Nothing was promised and nothing is owed —
    // retry on the next tick, exactly as a producer would after NOT_LEADER.
    schedule_append();
    return;
  }

  // **I2 — offsets are monotonic.** A regression here means recovery handed back a
  // next_offset below something already committed.
  if (assigned.value() < committed_before) {
    oracle_.violation("I2", describe(node_, "offset went backwards after recovery",
                                     assigned.value(), committed_before));
    return;
  }
  schedule_append();
}

// ---- BrokerObserver: watching, never deciding ----

void NodeWorkload::on_log_recovered(base::u64 next_offset, base::u64 bytes_truncated) {
  scheduler_.record(EventTag{EventKind::kRecovered, node_, next_offset, bytes_truncated});
  verify_recovery();
}

void NodeWorkload::on_raft_recovered(raft::Term term, raft::NodeId voted_for) {
  oracle_.observe_term(term);
  scheduler_.record(EventTag{EventKind::kRaftRecover, node_, term, voted_for});
  scheduler_.record(EventTag{EventKind::kRestart, node_, 0, 0});
}

void NodeWorkload::on_appended(base::u64 base_offset, base::u32 records) {
  scheduler_.record(EventTag{EventKind::kAppend, node_, base_offset, records});
}

void NodeWorkload::on_fsynced(base::u64 next_offset) {
  scheduler_.record(EventTag{EventKind::kFsync, node_, next_offset, 0});
}

void NodeWorkload::on_replicated(base::u64 next_offset) {
  scheduler_.record(EventTag{EventKind::kReplicate, node_, next_offset, 0});
}

void NodeWorkload::on_truncated(base::u64 from) {
  scheduler_.record(EventTag{EventKind::kTruncate, node_, from, 0});
}

void NodeWorkload::on_committed(base::u64 from, base::u64 to) {
  // Everything newly below the commit index is a promise the cluster has now made,
  // recorded once cluster-wide by whichever node noticed first. The oracle outlives the
  // leader that made it, which is the entire point of the promise being checkable after
  // that leader has been destroyed by a crash.
  oracle_.record_commit(to);
  for (base::u64 offset = from; offset < to; ++offset) {
    scheduler_.record(EventTag{EventKind::kCommit, node_, offset, 0});
  }
}

void NodeWorkload::on_campaign(raft::Term term) {
  oracle_.observe_term(term);
  scheduler_.record(EventTag{EventKind::kCampaign, node_, term, 0});
}

void NodeWorkload::on_vote(raft::Term term, raft::NodeId candidate) {
  oracle_.observe_term(term);
  scheduler_.record(EventTag{EventKind::kVote, node_, term, candidate});
}

void NodeWorkload::on_became_leader(raft::Term term, base::u64 log_end) {
  oracle_.observe_term(term);
  scheduler_.record(EventTag{EventKind::kLeader, node_, term, 0});
  oracle_.record_leader(term, node_);
  // The moment §5.4.1 promises this node holds every committed entry. If it does not, the
  // election safety argument is broken, not merely the data.
  oracle_.check_leader_completeness(node_, log_end);
}

void NodeWorkload::on_stepped_down(raft::Term term, raft::NodeId leader) {
  oracle_.observe_term(term);
  scheduler_.record(EventTag{EventKind::kStepDown, node_, term, leader});
}

void NodeWorkload::on_hard_state_persisted(raft::Term term, raft::NodeId voted_for) {
  scheduler_.record(EventTag{EventKind::kRaftPersist, node_, term, voted_for});
}

void NodeWorkload::on_protocol_error(const char* what, base::u64 a, base::u64 b) {
  oracle_.violation("FRAMING", describe(node_, what, a, b));
}

void NodeWorkload::verify_recovery() {
  // **I1 and I4, in their replicated form.**
  //
  // Week 4 could make a much stronger local claim: every node acked its own writes, so its
  // log had to reach its own highest acked offset or something was lost. With replication
  // that is no longer true of any individual node — a follower is *supposed* to lag.
  //
  // What is still true of every node, always, is that whatever committed prefix it does
  // hold must be **byte-identical** to what the cluster committed. A follower may be
  // short; it may never be different. That is the Log Matching Property (§5.3) checked
  // from outside, and it catches truncation-to-the-wrong-place, a replicated batch landing
  // at the wrong offset, and silent corruption of committed data.
  //
  // The other half — a *leader* may not be short — is checked at election time by
  // Oracle::check_leader_completeness(), because that is the moment §5.4.1 promises it.
  storage::Log* log = broker_ != nullptr ? broker_->log() : nullptr;
  if (log == nullptr) return;

  const base::u64 verifiable = std::min(oracle_.committed_end(), log->next_offset());
  if (verifiable == 0) return;

  // Only the tail. Recovery always yields a prefix (week 2's property test establishes
  // that independently, over every truncation point), so re-reading an hour of records on
  // every restart would make the run quadratic and prove nothing extra.
  const base::u64 window = static_cast<base::u64>(config_.verify_tail_batches) *
                           std::max(config_.records_per_batch, 1u);
  base::u64 offset = verifiable > window ? verifiable - window : 0;
  if (offset < log->start_offset()) offset = log->start_offset();

  std::vector<base::u8> buffer(64u * 1024u);
  while (offset < verifiable) {
    auto got = log->read(offset, base::MutSlice(buffer.data(), buffer.size()));
    if (!got && got.error() == base::ErrorCode::kIoError) {
      // The disk declined to answer. Not evidence anything is gone, and a checker that
      // cries wolf gets ignored exactly once. Inconclusive, not violated.
      return;
    }
    if (!got || got.value() == 0) {
      oracle_.violation("I1", describe(node_, "committed offset is unreadable after recovery",
                                       offset, verifiable));
      return;
    }

    const Slice framed(buffer.data(), got.value());
    auto header = storage::decode_header(framed);
    if (!header) {
      oracle_.violation("I1", describe(node_, "committed offset holds an undecodable batch",
                                       offset, 0));
      return;
    }

    storage::RecordIterator records(framed);
    Slice value;
    base::u64 at = header.value().base_offset;
    while (records.next(&value)) {
      if (at >= verifiable) break;
      if (at >= offset && value != Slice::from_string(payload_for(at))) {
        oracle_.violation("I1", describe(node_, "committed record holds bytes never written",
                                         at, 0));
        return;
      }
      ++at;
    }
    if (at <= offset) break;  // no forward progress; stop rather than spin
    offset = at;
  }
}

}  // namespace sim
