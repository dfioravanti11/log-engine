#pragma once

#include <map>
#include <set>
#include <utility>
#include <vector>

#include "io/random.h"
#include "raft/types.h"

namespace raft {

// The Raft state machine — election half (week 4). Replication is week 5.
//
// **This class touches nothing.** No clock, no socket, no disk, not even an `io::Disk`
// injected at construction — the only interface it holds is `io::Random`, for election
// timeout jitter. It is a pure function of (state, input): you `tick()` it, you `step()`
// messages into it, and you take a `Ready` out. Every effect on the world is something
// the *driver* does on its behalf.
//
// That is a stronger position than ER-1 requires, and it was chosen for three reasons:
//
//   1. A three-node election can be unit-tested with three of these in a `for` loop and
//      no infrastructure whatsoever — no event loop, no simulated disk, no time. Those
//      tests run in microseconds and pin down the algorithm, so when the simulator later
//      reports a failure the question is "which layer", not "where do I even start".
//   2. §13's rule — persist before you respond — becomes structural. The state machine
//      cannot send anything; it can only ask. The driver holds the one code path where
//      the ordering could be got wrong, so there is one place to review.
//   3. Timeouts are counted in ticks, so the clock-jump fault has no route into Raft.
//      Correctness never depends on the wall clock (§17), and here it provably cannot.
//
// Ticks, not deadlines, is the etcd design rather than the one in the Raft paper's
// pseudocode. The trade is resolution: a timeout is only accurate to one tick. At a 10 ms
// tick against a 150 ms election timeout that is under 7% of error, which the jitter
// dwarfs anyway.
class Node {
 public:
  // `persisted` is what was recovered from `raft.state`, or a default-constructed
  // HardState for a node that has never voted. Passing it through the constructor rather
  // than a `restore()` call means there is no window in which the node exists with the
  // wrong term.
  Node(Config config, io::Random& rng, HardState persisted);

  // One unit of time. The driver calls this from a repeating timer.
  void tick();

  // Feed in a message from a peer.
  void step(const Message& message);

  // Start an election immediately, without waiting for the timeout. Used by tests and
  // by nothing else — production elections are always timeout-driven.
  void campaign();

  // Drains the pending effects. See Ready: persist first, then send.
  //
  // Taking a Ready does **not** clear the hard-state debt — `hard_state_persisted()`
  // does, and only the driver knows when that is true.
  [[nodiscard]] Ready take_ready();

  // Call after the fsync of `raft.state` has returned, and not before.
  //
  // The debt is deliberately sticky, because the two ways of getting this wrong are not
  // symmetric. Forget to call this and the node rewrites 32 bytes it had already written
  // — wasteful, harmless. Clear it optimistically inside `take_ready()` (which is what
  // this class did first) and a failed write leaves the node with a vote it believes it
  // recorded and never did: the next request from the same candidate is granted with no
  // state change to report, so nothing downstream asks for an fsync, and a crash after
  // that is the amnesia of §13 with every layer behaving exactly as written.
  void hard_state_persisted() noexcept { hard_state_dirty_ = false; }

  // ---- The log, as seen from the consensus layer ----
  //
  // The node never holds an entry. It holds the log's exclusive end and an append-only
  // map of which term produced which range of indices — enough to answer the two
  // questions Raft actually asks of a log ("how current is yours?" and "what term is the
  // entry at index N?") without the entries being here at all. That map is the leader
  // epoch cache of §12.3, and on a real node it is rebuilt at startup by scanning batch
  // headers, which already carry `leader_epoch`.

  // Called by the driver at startup with what recovery found, and after it truncates.
  // `epochs` must be ascending in both fields and is trusted; it comes from the log.
  void restore_log(Index log_end, const std::vector<Epoch>& epochs);

  // The driver appended one batch locally: it starts at `index`, ends at `end`
  // (exclusive), and was stamped with `term`.
  //
  // `end` is not `index + 1`. An entry is a batch and a batch holds many records, each
  // with its own offset (§12.2), so the log's end advances by the record count and not by
  // one. Getting that wrong makes every later index land in the middle of a batch, where
  // no read can start.
  void log_appended(Index index, Index end, Term term);

  [[nodiscard]] Index log_end() const noexcept { return log_end_; }
  [[nodiscard]] Term last_term() const noexcept { return last_term_; }
  [[nodiscard]] Index commit_index() const noexcept { return commit_index_; }

  // The term of the entry at `index`, or `kNoTerm` if this log has no such entry.
  [[nodiscard]] Term term_at(Index index) const noexcept;

  // A leader's view of one follower: how much of its log we have confirmed matches.
  // Exposed for tests and for the simulator's invariant checker, not for the driver.
  [[nodiscard]] Index match_end(NodeId peer) const noexcept;
  [[nodiscard]] Index next_index(NodeId peer) const noexcept;

  [[nodiscard]] Role role() const noexcept { return role_; }
  [[nodiscard]] bool is_leader() const noexcept { return role_ == Role::kLeader; }
  [[nodiscard]] Term term() const noexcept { return hard_.term; }
  [[nodiscard]] NodeId voted_for() const noexcept { return hard_.voted_for; }
  [[nodiscard]] NodeId leader() const noexcept { return leader_; }
  [[nodiscard]] NodeId id() const noexcept { return config_.id; }
  [[nodiscard]] const Config& config() const noexcept { return config_; }
  [[nodiscard]] std::size_t votes() const noexcept { return votes_.size(); }

  // How many elections this node has started. A term that climbs without anybody
  // winning is the signature of a split vote or a partitioned node, and it is invisible
  // in any "is there a leader" check.
  [[nodiscard]] base::u64 campaigns() const noexcept { return campaigns_; }

  // Vote requests turned away because this node still had a live leader (§4.2.3). Zero
  // here across a run with partitions would mean the rule is not firing, and the
  // livelock it prevents would be back without any test noticing.
  [[nodiscard]] base::u64 lease_refusals() const noexcept { return lease_refusals_; }

 private:
  void become_follower(Term term, NodeId leader);
  void become_candidate();
  void become_leader();

  void handle_request_vote(const Message& message);
  void handle_request_vote_response(const Message& message);
  void handle_append_entries(const Message& message);
  void handle_append_entries_response(const Message& message);

  // Builds the next AppendEntries for one peer: a heartbeat if the peer is caught up,
  // otherwise the single entry at its `next_index`, with the preceding entry's index and
  // term attached for the §5.3 match check.
  void send_append_entries(NodeId peer);
  // First index of the epoch containing `index` — always a batch boundary, because an
  // epoch only ever begins where a batch begins.
  [[nodiscard]] Index epoch_start_at(Index index) const noexcept;
  void append_epoch(Term term, Index start);
  void truncate_epochs_from(Index index);

  // Raft §5.3/§5.4.2: an index is committed once a majority has it *and* it was produced
  // in the current term. Committing on the majority alone is the classic unsafe shortcut —
  // it can commit an entry from an old term that a future leader is entitled to discard.
  void advance_commit_index();

  void broadcast_heartbeat();
  void reset_election_timer();
  void send(Message message);
  void reply(const Message& to, MessageType type, bool granted);

  // Raft §5.4.1: a candidate's log must be at least as current as the voter's, or the
  // voter refuses. This is the entire reason a leader is guaranteed to hold every
  // committed entry.
  [[nodiscard]] bool log_is_up_to_date(Index index, Term term) const noexcept;

  Config config_;
  io::Random& rng_;

  HardState hard_;
  bool hard_state_dirty_ = false;

  Role role_ = Role::kFollower;
  NodeId leader_ = kNoNode;

  // Sorted, not hashed. Iteration order feeds nothing here today, but ER-2 is a rule
  // about not having to check that claim again every time the code changes.
  std::set<NodeId> votes_;

  // The log, as metadata only. `log_end_` is exclusive: 0 is an empty log.
  Index log_end_ = 0;
  Term last_term_ = kNoTerm;
  Index commit_index_ = 0;
  std::vector<Epoch> epochs_;

  // Leader state, per follower. Both are exclusive ends: `match` is how much of our log
  // we have confirmed the peer holds, `next` is where we will send from. Ordered maps —
  // these are iterated during commit-index arithmetic, and ER-2 forbids that depending on
  // a hash order.
  std::map<NodeId, Index> match_;
  std::map<NodeId, Index> next_;

  // Set while handling an AppendEntries, drained into the next Ready.
  Index pending_truncate_ = kNoIndex;
  bool pending_append_ = false;

  base::u32 election_elapsed_ = 0;
  base::u32 heartbeat_elapsed_ = 0;
  base::u32 election_timeout_ = 0;  // randomized, re-drawn on every reset

  base::u64 campaigns_ = 0;
  base::u64 lease_refusals_ = 0;
  std::vector<Message> out_;
};

}  // namespace raft
