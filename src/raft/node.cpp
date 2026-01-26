#include "raft/node.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace raft {

const char* to_string(Role role) noexcept {
  switch (role) {
    case Role::kFollower: return "follower";
    case Role::kCandidate: return "candidate";
    case Role::kLeader: return "leader";
  }
  return "unknown";
}

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::kRequestVote: return "RequestVote";
    case MessageType::kRequestVoteResponse: return "RequestVoteResp";
    case MessageType::kAppendEntries: return "AppendEntries";
    case MessageType::kAppendEntriesResponse: return "AppendEntriesResp";
  }
  return "Unknown";
}

Node::Node(Config config, io::Random& rng, HardState persisted)
    : config_(std::move(config)), rng_(rng), hard_(persisted) {
  // Recovered state is by definition already durable, so the node does not owe a write
  // for it. Marking it dirty here would fsync once per restart for no reason — harmless,
  // but it would also mean the "did this node persist before responding" test could pass
  // by accident.
  reset_election_timer();
}

void Node::reset_election_timer() {
  election_elapsed_ = 0;
  const base::u32 jitter =
      config_.election_timeout_jitter_ticks == 0
          ? 0
          : static_cast<base::u32>(rng_.next_below(config_.election_timeout_jitter_ticks));
  election_timeout_ = config_.election_timeout_ticks + jitter;
  if (election_timeout_ == 0) election_timeout_ = 1;
}

void Node::send(Message message) {
  message.from = config_.id;
  message.term = hard_.term;
  out_.push_back(message);
}

void Node::reply(const Message& to, MessageType type, bool granted) {
  Message response;
  response.type = type;
  response.to = to.from;
  response.granted = granted;
  // Our log end, on every response. On an accept it tells the leader exactly how much we
  // now hold; on a reject it tells it where to resume from, so backing off costs one
  // round trip rather than one per index.
  response.log_index = log_end_;
  response.log_term = last_term_;
  send(response);
}

bool Node::log_is_up_to_date(Index end, Term term) const noexcept {
  if (term != last_term_) return term > last_term_;
  return end >= log_end_;
}

Term Node::term_at(Index index) const noexcept {
  if (index >= log_end_ || epochs_.empty()) return kNoTerm;
  // The last epoch whose start is <= index. Binary search, because this runs once per
  // AppendEntries and a busy log accumulates an epoch per leader change.
  const auto it = std::upper_bound(
      epochs_.begin(), epochs_.end(), index,
      [](Index target, const Epoch& epoch) { return target < epoch.start; });
  if (it == epochs_.begin()) return kNoTerm;  // index predates the log we still hold
  return std::prev(it)->term;
}

Index Node::epoch_start_at(Index index) const noexcept {
  if (epochs_.empty()) return 0;
  const auto it = std::upper_bound(
      epochs_.begin(), epochs_.end(), index,
      [](Index target, const Epoch& epoch) { return target < epoch.start; });
  if (it == epochs_.begin()) return epochs_.front().start;
  return std::prev(it)->start;
}

void Node::append_epoch(Term term, Index start) {
  // Only a *change* of term opens an epoch. Consecutive entries from one leader share one.
  if (!epochs_.empty() && epochs_.back().term == term) return;
  epochs_.push_back(Epoch{term, start});
}

void Node::truncate_epochs_from(Index index) {
  while (!epochs_.empty() && epochs_.back().start >= index) epochs_.pop_back();
}

void Node::restore_log(Index log_end, const std::vector<Epoch>& epochs) {
  log_end_ = log_end;
  epochs_ = epochs;
  last_term_ = log_end_ == 0 ? kNoTerm : term_at(log_end_ - 1);
  if (commit_index_ > log_end_) commit_index_ = log_end_;
}

void Node::log_appended(Index index, Index end, Term term) {
  append_epoch(term, index);
  log_end_ = end > index ? end : index + 1;
  last_term_ = term;

  if (role_ != Role::kLeader) return;

  // A single-node cluster is its own quorum, so its own append commits immediately.
  advance_commit_index();

  // Push it out now rather than waiting for the next heartbeat — that is the difference
  // between commit latency being one round trip and being one heartbeat interval. Only to
  // peers that were already caught up: a peer still catching up is being fed by the
  // response loop in handle_append_entries_response(), and sending here as well would
  // duplicate every entry of its backlog.
  for (NodeId peer : config_.peers) {
    if (next_index(peer) == index) send_append_entries(peer);
  }
}

Index Node::match_end(NodeId peer) const noexcept {
  const auto it = match_.find(peer);
  return it == match_.end() ? 0 : it->second;
}

Index Node::next_index(NodeId peer) const noexcept {
  const auto it = next_.find(peer);
  return it == next_.end() ? 0 : it->second;
}

void Node::advance_commit_index() {
  if (role_ != Role::kLeader) return;

  // Every log end that some member is known to hold, this node included.
  std::vector<Index> ends;
  ends.reserve(config_.cluster_size());
  ends.push_back(log_end_);
  for (NodeId peer : config_.peers) ends.push_back(match_end(peer));

  // The highest end that a quorum has reached: sort descending and take the quorum-th.
  std::sort(ends.begin(), ends.end(), std::greater<Index>());
  const Index quorum_end = ends[config_.quorum() - 1];
  if (quorum_end <= commit_index_) return;

  // **Raft §5.4.2.** A majority holding an entry is not sufficient to commit it if it
  // came from an earlier term — a later leader may legitimately overwrite it, and
  // committing it here would be promising something that can still be taken back. Only
  // entries from the leader's own term may be committed directly; older ones ride along
  // once one of them is.
  if (term_at(quorum_end - 1) != hard_.term) return;

  commit_index_ = quorum_end;
}

void Node::become_follower(Term term, NodeId leader) {
  if (term > hard_.term) {
    // A new term wipes the vote. Both fields move together and both must be durable
    // before this node answers anything, which is why they are one struct (§13).
    hard_.term = term;
    hard_.voted_for = kNoNode;
    hard_state_dirty_ = true;
  }
  role_ = Role::kFollower;
  leader_ = leader;
  votes_.clear();
  reset_election_timer();
}

void Node::become_candidate() {
  hard_.term += 1;
  hard_.voted_for = config_.id;  // a candidate always votes for itself
  hard_state_dirty_ = true;

  role_ = Role::kCandidate;
  leader_ = kNoNode;
  votes_.clear();
  votes_.insert(config_.id);
  ++campaigns_;
  reset_election_timer();
}

void Node::become_leader() {
  role_ = Role::kLeader;
  leader_ = config_.id;
  heartbeat_elapsed_ = 0;

  // Optimistic, exactly as the paper says: assume every follower matches us and walk back
  // one step per rejection. The alternative — start at zero and walk forward — is correct
  // but re-ships the entire log after every election.
  match_.clear();
  next_.clear();
  for (NodeId peer : config_.peers) {
    match_[peer] = 0;
    next_[peer] = log_end_;
  }

  broadcast_heartbeat();
}

void Node::send_append_entries(NodeId peer) {
  const Index from = next_index(peer);

  Message message;
  message.type = MessageType::kAppendEntries;
  message.to = peer;
  message.commit_index = commit_index_;

  // The entry before the one we are about to send. `kNoIndex` says "yours should be empty
  // here" — the only honest way to express "there is no predecessor" when index 0 is a
  // real entry.
  if (from == 0) {
    message.log_index = kNoIndex;
    message.log_term = kNoTerm;
  } else {
    message.log_index = from - 1;
    message.log_term = term_at(from - 1);
  }

  // One entry per message (§16.2 — a batch *is* an entry). The node names the index; the
  // driver reads that batch out of storage and attaches the bytes.
  if (from < log_end_) {
    message.entry_index = from;
    message.entry_term = term_at(from);
  }

  send(message);
}

void Node::broadcast_heartbeat() {
  for (NodeId peer : config_.peers) send_append_entries(peer);
}

void Node::campaign() {
  become_candidate();

  // A single-node cluster is its own quorum and wins before sending anything. Worth
  // handling explicitly: it is the degenerate case every off-by-one in quorum() shows up
  // in, and the simulator runs node_count=1 on purpose.
  if (votes_.size() >= config_.quorum()) {
    become_leader();
    return;
  }

  for (NodeId peer : config_.peers) {
    Message message;
    message.type = MessageType::kRequestVote;
    message.to = peer;
    message.log_index = log_end_;
    message.log_term = last_term_;
    send(message);
  }
}

void Node::tick() {
  if (role_ == Role::kLeader) {
    ++heartbeat_elapsed_;
    if (heartbeat_elapsed_ >= config_.heartbeat_timeout_ticks) {
      heartbeat_elapsed_ = 0;
      broadcast_heartbeat();
    }
    return;
  }

  ++election_elapsed_;
  if (election_elapsed_ >= election_timeout_) campaign();
}

void Node::step(const Message& message) {
  // **The disruptive-server rule** (Raft dissertation §4.2.3), and it has to come before
  // the term rule below, not after — because adopting the term is what destroys the very
  // evidence this check needs.
  //
  // A follower that has heard from its leader inside a full election timeout refuses to
  // consider a vote at all, and refuses *without* adopting the candidate's term. Without
  // this, a node that can still reach the leader will cheerfully depose it on behalf of
  // some third node that cannot — see bug journal #2, where one cut link made leadership
  // ping-pong every 200 ms for as long as the partition lasted.
  //
  // No reply is sent. A rejection carrying a lower term is discarded as stale by the
  // candidate anyway, so it would be pure noise on a link that is already struggling.
  if (message.type == MessageType::kRequestVote && role_ == Role::kFollower &&
      leader_ != kNoNode && election_elapsed_ < config_.election_timeout_ticks) {
    ++lease_refusals_;
    return;
  }

  // Rule one, before anything else is considered (Raft §5.1): a message carrying a
  // higher term makes this node a follower of that term, whatever it was doing.
  if (message.term > hard_.term) {
    // A RequestVote says nothing about who the leader is — a candidate is not a leader,
    // and recording it as one would send clients to a node that may well lose.
    const NodeId hint =
        message.type == MessageType::kAppendEntries ? message.from : kNoNode;
    become_follower(message.term, hint);
  }

  if (message.term < hard_.term) {
    // Stale sender. Answering — rather than dropping — is what makes it step down on the
    // spot instead of waiting out its own election timeout, and a partitioned node that
    // rejoins with an old term is the common case, not an exotic one.
    switch (message.type) {
      case MessageType::kRequestVote:
        reply(message, MessageType::kRequestVoteResponse, false);
        break;
      case MessageType::kAppendEntries:
        reply(message, MessageType::kAppendEntriesResponse, false);
        break;
      case MessageType::kRequestVoteResponse:
      case MessageType::kAppendEntriesResponse:
        break;  // a stale response is just noise
    }
    return;
  }

  switch (message.type) {
    case MessageType::kRequestVote: handle_request_vote(message); break;
    case MessageType::kRequestVoteResponse: handle_request_vote_response(message); break;
    case MessageType::kAppendEntries: handle_append_entries(message); break;
    case MessageType::kAppendEntriesResponse:
      handle_append_entries_response(message);
      break;
  }
}

void Node::handle_request_vote(const Message& message) {
  // One vote per term, and it is remembered across a crash or it means nothing. Voting
  // for the same candidate twice is fine and necessary — a lost response makes the
  // candidate retry, and refusing the retry would cost an election for no reason.
  const bool already_committed_elsewhere =
      hard_.voted_for != kNoNode && hard_.voted_for != message.from;
  const bool granted =
      !already_committed_elsewhere && log_is_up_to_date(message.log_index, message.log_term);

  if (granted) {
    // Only a *change* owes a write. A candidate whose response was lost retries every
    // election timeout, and re-fsyncing a byte-identical `raft.state` on each retry would
    // put a stall on the durability path at exactly the moment the cluster is already
    // struggling. If the previous write failed, the debt is still outstanding anyway —
    // it is sticky until the driver says otherwise.
    if (hard_.voted_for != message.from) {
      hard_.voted_for = message.from;
      hard_state_dirty_ = true;
    }
    // Having just endorsed someone, do not turn around and campaign against them. This
    // is the line that keeps a healthy election from being disrupted by the very nodes
    // that voted in it.
    election_elapsed_ = 0;
  }
  reply(message, MessageType::kRequestVoteResponse, granted);
}

void Node::handle_request_vote_response(const Message& message) {
  if (role_ != Role::kCandidate) return;  // already won, already lost, or already moved on
  if (!message.granted) return;

  votes_.insert(message.from);
  if (votes_.size() >= config_.quorum()) become_leader();
}

void Node::handle_append_entries(const Message& message) {
  if (role_ == Role::kLeader) {
    // Two leaders in one term. The term rules make this unreachable, and if it ever does
    // happen the bug is upstream of here — so this node does not try to repair it by
    // stepping down (two leaders would then ping-pong forever, hiding the cause). The
    // simulator's I6 check catches it globally, with a seed.
    return;
  }

  // A candidate that hears from a leader in its own term has lost; there is no waiting
  // to see. Terms are equal here, so this cannot clear the vote already cast.
  become_follower(message.term, message.from);

  // **The log-matching check (Raft §5.3).** The leader claims the entry before the one it
  // is sending sits at `log_index` with term `log_term`. If this log disagrees — wrong
  // term there, or nothing there at all — then everything from that point on is suspect
  // and the append is refused. The leader walks back and tries an earlier index.
  const bool wants_first_entry = message.log_index == kNoIndex;
  const bool matches = wants_first_entry
                           ? true
                           : (message.log_index < log_end_ &&
                              term_at(message.log_index) == message.log_term);

  if (!matches) {
    // **The rejection carries a hint, and the hint must be a batch boundary.**
    //
    // Classic Raft has the leader back off one index per round trip. That is already slow,
    // and here it is also *wrong*: an index is a record offset, an entry is a batch of
    // several records, and `index - 1` usually lands in the middle of one — where no read
    // can start and no append can land. The leader has no idea where batches begin; only
    // the log's owner does.
    //
    // So the follower answers with a place it knows is a boundary. If the leader ran off
    // the end of our log, that is our log end. If it disagreed about a term, it is the
    // first index of the epoch it disagreed in — everything in that epoch is suspect
    // anyway, so this skips the whole run in one round trip rather than one entry at a
    // time (the conflicting-term optimization, dissertation §5.3).
    Message response;
    response.type = MessageType::kAppendEntriesResponse;
    response.to = message.from;
    response.granted = false;
    response.log_term = last_term_;
    response.log_index = message.log_index >= log_end_ ? log_end_
                                                       : epoch_start_at(message.log_index);
    send(response);
    return;
  }

  const Index append_at = wants_first_entry ? 0 : message.log_index + 1;

  // How far this entry reaches. `entry_end` is filled in by the sender's *driver*, which
  // is the only thing that knows how many records a batch holds — so a sender that does
  // not track batch extents at all (every unit test in `test_raft_replication.cpp`, and
  // any future caller with one record per entry) simply leaves it unset, and one record
  // is the right reading. Trusting a zero here would make the entry span nothing, the
  // response confirm nothing, and the leader resend it forever.
  const Index entry_end = message.entry_end > message.entry_index ? message.entry_end
                                                                  : message.entry_index + 1;

  if (message.carries_entry()) {
    // Anything at or past this point either duplicates what is arriving or belongs to a
    // leader that lost. Either way it goes, and it goes *before* the append — which is
    // why Ready states them in that order and the driver may not reorder them.
    //
    // Truncating only when the tail actually disagrees matters: a duplicate AppendEntries
    // is routine (the leader retries whenever a response is lost), and throwing away a
    // correct entry to write the identical bytes back would turn every retry into a
    // rewrite of the log's tail.
    const Term existing = term_at(append_at);
    if (existing != kNoTerm && existing != message.entry_term) {
      pending_truncate_ = append_at;
      truncate_epochs_from(append_at);
      log_end_ = append_at;
      last_term_ = log_end_ == 0 ? kNoTerm : term_at(log_end_ - 1);
    }

    if (append_at >= log_end_) {
      pending_append_ = true;
      log_appended(append_at, entry_end, message.entry_term);
    }
  } else if (append_at < log_end_) {
    // A heartbeat that names a predecessor we agree on tells us nothing about what comes
    // after it, so nothing is truncated here. The leader will send the real entry next.
  }

  // The follower adopts the leader's commit index, clamped to what it actually holds —
  // it must never claim to have committed an entry it does not have (I5).
  if (message.commit_index > commit_index_) {
    commit_index_ = std::min(message.commit_index, log_end_);
  }

  // **Report what matched, not how long this log is.**
  //
  // Those are different numbers, and confusing them is a safety bug rather than an
  // accounting one. A follower can hold a *longer* log than the leader — a divergent tail
  // from a term that lost — and a heartbeat that agrees about the entry at `prev` says
  // nothing whatsoever about the entries after it. Replying with `log_end_` would tell the
  // leader that all of them match, so `match_` for this follower would run past the
  // leader's own log, and the quorum arithmetic would count entries the leader has never
  // seen toward committing entries it has.
  //
  // What is actually confirmed is everything up to and including the entry just accepted,
  // or up to `prev` if this was a bare heartbeat.
  Message response;
  response.type = MessageType::kAppendEntriesResponse;
  response.to = message.from;
  response.granted = true;
  response.log_index = message.carries_entry() ? entry_end : append_at;
  response.log_term = last_term_;
  send(response);
}

void Node::handle_append_entries_response(const Message& message) {
  if (role_ != Role::kLeader) return;

  if (!message.granted) {
    // Take the follower's hint. It is a batch boundary by construction — see the comment
    // where it is produced — and clamping it here keeps a stale or malicious response from
    // undoing progress we have already confirmed, or from pointing past our own log.
    const Index floor = match_end(message.from);
    Index retry = message.log_index;
    if (retry > log_end_) retry = log_end_;
    if (retry < floor) retry = floor;
    next_[message.from] = retry;
    send_append_entries(message.from);
    return;
  }

  // Never past our own log: a peer cannot have confirmed more of our log than exists, and
  // letting `match_` exceed `log_end_` would put entries we do not hold into the quorum
  // arithmetic below.
  const Index confirmed = std::min(message.log_index, log_end_);
  if (confirmed > match_end(message.from)) match_[message.from] = confirmed;
  if (confirmed > next_index(message.from)) next_[message.from] = confirmed;

  advance_commit_index();

  // Still behind: keep feeding it rather than waiting for the next heartbeat tick. This
  // is what makes catch-up take one round trip per entry instead of one heartbeat.
  if (next_index(message.from) < log_end_) send_append_entries(message.from);
}

Ready Node::take_ready() {
  Ready ready;
  ready.persist_hard_state = hard_state_dirty_;
  ready.hard_state = hard_;
  ready.messages.swap(out_);
  ready.truncate_from = pending_truncate_;
  ready.append_entry = pending_append_;
  ready.commit_index = commit_index_;
  // The hard-state debt survives this call on purpose. Only the driver knows whether the
  // write landed, so only the driver may clear it — see hard_state_persisted().
  //
  // The log instructions are different and are cleared here: they describe the message
  // that was just stepped, and if the driver fails to carry them out the follower simply
  // rejects the leader's next AppendEntries and gets sent the same entry again. Raft's
  // retry loop already covers that; there is nothing to remember.
  pending_truncate_ = kNoIndex;
  pending_append_ = false;
  return ready;
}

}  // namespace raft
