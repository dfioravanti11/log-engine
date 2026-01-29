#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/buffer.h"
#include "io/disk.h"
#include "io/network.h"
#include "io/random.h"
#include "raft/node.h"
#include "raft/types.h"
#include "server/broker.h"
#include "runtime/event_loop.h"
#include "sim/scheduler.h"
#include "storage/log.h"
#include "wire/frame.h"

namespace sim {

// One batch the cluster promised a producer: replicated to a majority and committed.
//
// Week 4 recorded these per node, because each node appended independently and a promise
// was a local fact. With replication a promise is a *cluster* fact — the leader that made
// it may be gone, and the entry is still owed.
struct AckedBatch {
  base::u64 base_offset = 0;
  base::u32 record_count = 0;
};

// The shadow model (§14.2). It lives in the simulator, never in a node, and that is the
// whole point: a node checking its own durability against its own memory would agree
// with itself after losing both. The oracle remembers what was promised across crashes
// that the node does not survive.
class Oracle {
 public:
  explicit Oracle(base::u32 node_count) : acked_(node_count) {}

  void record_ack(base::u32 node, AckedBatch batch) { acked_[node].push_back(batch); }
  [[nodiscard]] const std::vector<AckedBatch>& acks(base::u32 node) const {
    return acked_[node];
  }
  [[nodiscard]] base::u64 total_batches() const;

  // ---- Replication (week 5) ----

  // The cluster committed everything below `end`. Monotonic by construction; a commit
  // index that went backwards would itself be the violation.
  void record_commit(base::u64 end);
  [[nodiscard]] base::u64 committed_end() const noexcept { return committed_end_; }
  [[nodiscard]] base::u64 commits() const noexcept { return commits_; }

  // **I1, in its replicated form.** A leader is guaranteed to hold every committed entry
  // (Raft §5.4.1) — that guarantee is the entire reason the up-to-date check exists. So a
  // node that wins an election with a log shorter than the committed prefix has not just
  // lost data, it has proved the election safety argument broken.
  //
  // Checked at the moment leadership is claimed, because that is when the guarantee is
  // supposed to hold and because a leader is the one node whose log is about to become
  // everybody else's.
  void check_leader_completeness(base::u32 node, base::u64 log_end);

  // **I8 — liveness.** Journal #2's debt: I1–I6 are all safety properties, and a cluster
  // that does nothing satisfies every one of them. A thousand green seeds said nothing
  // about a cluster spending thirty seconds at a time electing and committing nothing.
  //
  // Stated conditionally, because the unconditional version is false: during a partition
  // that costs the majority, having no leader is *correct*. So the simulator reports the
  // longest stretch with no leader anywhere, and the caller decides what is tolerable
  // given the faults it injected. "A leader exists" is not the invariant; "the cluster
  // gets one back within a bounded time of being able to" is.
  void note_leader_present(base::Nanos at);
  void note_leader_absent(base::Nanos at);

  // The cluster currently has no majority alive, so having no leader is **correct** and
  // the stretch in progress is not a failover. Called while leaderless; it disqualifies
  // the current gap from the failover distribution without ending it.
  //
  // This is the same conditionality I8 is stated with, applied to a measurement instead of
  // an invariant — and it had to be learned twice. The first failover run excluded
  // partitions for exactly this reason and then folded in every stretch where two of three
  // nodes were down, producing a p99 of 2.9 s that was really `restart_delay_max` wearing
  // a benchmark's clothes.
  void note_quorum_lost() noexcept { gap_had_quorum_ = false; }
  [[nodiscard]] base::Nanos longest_leaderless() const noexcept { return longest_gap_; }

  // Every leaderless stretch, not just the longest — this is **failover time**, one
  // sample per failure, and NFR-3 wants p50 and p99 across at least fifty of them. The
  // startup election is in here too; the benchmark drops the first sample per run,
  // because a cold start is not a failover.
  [[nodiscard]] const std::vector<base::Nanos>& leaderless_gaps() const noexcept {
    return gaps_;
  }

  // **I6 — at most one leader per term.** Called by a node the moment it counts a
  // majority, before it has told anybody.
  //
  // Held here rather than derived by asking every node "are you the leader?", because
  // that question only ever sees the present. A node can win a term, ack, and be
  // destroyed by a crash before any observer looks — and that leader still happened, and
  // still counts. The map remembers claims the cluster itself no longer can.
  void record_leader(raft::Term term, raft::NodeId node);
  [[nodiscard]] base::u64 elections() const noexcept { return elections_; }
  [[nodiscard]] raft::Term highest_term() const noexcept { return highest_term_; }
  void observe_term(raft::Term term) noexcept {
    if (term > highest_term_) highest_term_ = term;
  }

  // Offset one past the highest acked record, or 0 if nothing has been acked.
  [[nodiscard]] base::u64 acked_end(base::u32 node) const;
  [[nodiscard]] base::u64 total_records() const;

  // First violation wins: everything after the first broken invariant is a consequence,
  // and reporting the cascade buries the cause.
  void violation(const char* invariant, std::string detail);
  [[nodiscard]] bool ok() const noexcept { return invariant_ == nullptr; }
  [[nodiscard]] const char* invariant() const noexcept { return invariant_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

 private:
  std::vector<std::vector<AckedBatch>> acked_;
  // Ordered, so a violation report reads in term order and two runs of one seed produce
  // byte-identical output (ER-2).
  std::map<raft::Term, raft::NodeId> leader_by_term_;
  base::u64 elections_ = 0;
  raft::Term highest_term_ = 0;
  base::u64 committed_end_ = 0;
  base::u64 commits_ = 0;
  base::Nanos leaderless_since_ = 0;
  base::Nanos longest_gap_ = 0;
  std::vector<base::Nanos> gaps_;
  bool gap_had_quorum_ = true;
  bool leader_present_ = false;
  const char* invariant_ = nullptr;
  std::string detail_;
};

// Deterministic record contents, so the verifier recomputes what it should see instead
// of trusting a second copy of the data. Keyed on the offset alone: with replication
// there is one log, and every node must hold identical bytes at a given offset.
std::string payload_for(base::u64 offset);

// One simulated broker process: a real `server::Broker` on simulated I/O, plus the load
// generator and the invariant checking that only the simulator does.
//
// **This class owns a Broker; it is not a copy of one.** Weeks 3–5 grew the driver here,
// and week 6 moved it into `server/` unchanged so the production binary runs the same
// object. What is left in this file is everything that is *not* the broker: generating
// load, and watching — through `server::BrokerObserver`, which is observation only. A
// null observer changes nothing about what a broker does, which is what makes "the
// simulator tests the real code" a fact rather than a slogan.
//
// The object is destroyed and rebuilt on every crash, exactly like the process it stands
// in for. The disk it writes to is not.
class NodeWorkload final : public server::BrokerObserver {
 public:
  struct Config {
    base::Nanos append_interval = base::millis(40);
    base::Nanos append_jitter = base::millis(20);
    base::u32 records_per_batch = 2;
    base::u32 record_bytes = 48;
    // Recovery yields a prefix, so checking the tail proves the rest. See verify().
    std::size_t verify_tail_batches = 32;

    base::Nanos raft_tick = base::millis(10);
    base::u32 election_timeout_ticks = 15;
    base::u32 election_timeout_jitter_ticks = 15;
    base::u32 heartbeat_timeout_ticks = 5;

    // **Faults, not features**, both defaulting to the safe setting. They exist so the
    // simulator can break a durability guarantee on a named seed and show the consequence
    // (§13.2) — a checker nobody has watched fail is not evidence.
    bool fsync_hard_state = true;
    bool ack_on_local_append = false;
  };

  using Peer = server::BrokerConfig::Peer;

  NodeWorkload(base::u32 node, Scheduler& scheduler, runtime::EventLoop& loop,
               io::Disk& disk, io::Random& rng, Oracle& oracle, std::string dir,
               base::u16 port, std::vector<Peer> peers, Config config);
  ~NodeWorkload() override;

  base::Status start();

  [[nodiscard]] base::u64 appends() const noexcept;
  [[nodiscard]] base::u64 raft_messages_sent() const noexcept;
  [[nodiscard]] base::u64 hard_state_writes() const noexcept;
  [[nodiscard]] base::u64 ticks() const noexcept;
  [[nodiscard]] bool is_leader() const noexcept;

  // ---- server::BrokerObserver: trace and invariants, never decisions ----
  void on_log_recovered(base::u64 next_offset, base::u64 bytes_truncated) override;
  void on_raft_recovered(raft::Term term, raft::NodeId voted_for) override;
  void on_appended(base::u64 base_offset, base::u32 records) override;
  void on_fsynced(base::u64 next_offset) override;
  void on_replicated(base::u64 next_offset) override;
  void on_truncated(base::u64 from) override;
  void on_committed(base::u64 from, base::u64 to) override;
  void on_campaign(raft::Term term) override;
  void on_vote(raft::Term term, raft::NodeId candidate) override;
  void on_became_leader(raft::Term term, base::u64 log_end) override;
  void on_stepped_down(raft::Term term, raft::NodeId leader) override;
  void on_hard_state_persisted(raft::Term term, raft::NodeId voted_for) override;
  void on_protocol_error(const char* what, base::u64 a, base::u64 b) override;

 private:
  void verify_recovery();
  void schedule_append();
  void do_append();

  base::u32 node_;
  Scheduler& scheduler_;
  runtime::EventLoop& loop_;
  io::Disk& disk_;
  io::Random& rng_;
  Oracle& oracle_;
  std::string dir_;
  base::u16 port_;
  std::vector<Peer> peers_;
  Config config_;

  std::unique_ptr<server::Broker> broker_;
  storage::BatchBuilder builder_;
  bool running_ = false;
};

}  // namespace sim
