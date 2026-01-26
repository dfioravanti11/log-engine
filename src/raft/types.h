#pragma once

#include <cstddef>
#include <limits>
#include <vector>

#include "base/types.h"

namespace raft {

using NodeId = base::u32;
using Term = base::u64;
using Index = base::u64;

// "No node": no vote cast, or no leader known. A sentinel rather than std::optional so
// the type stays trivially copyable and encodes to a fixed-width field on the wire.
inline constexpr NodeId kNoNode = std::numeric_limits<NodeId>::max();

// "No index": no such entry. Used for "the log is empty before this point" and for "this
// message carries no entry".
//
// **Indices here are storage offsets**, so they start at 0 and the log is described by an
// *exclusive end* — `log_end == 0` is an empty log, and the last entry, when there is one,
// sits at `log_end - 1`. The Raft paper starts its log at index 1 and uses 0 for "empty",
// which would mean maintaining a translation at every boundary between `raft/` and
// `storage/` for no benefit. Exclusive ends also make the quorum arithmetic in
// `advance_commit_index()` read without an off-by-one in it.
inline constexpr Index kNoIndex = std::numeric_limits<Index>::max();

// Term 0 is "no term" — no entry is ever produced in it, because a leader must win an
// election first and the first election is term 1.
inline constexpr Term kNoTerm = 0;

enum class Role : base::u8 { kFollower = 0, kCandidate = 1, kLeader = 2 };

// One run of consecutive indices produced by a single leader — the leader epoch cache of
// §12.3, which `storage/` rebuilds by scanning batch headers on startup.
//
// Append-only and binary-searched. It is the reason `raft::Node` can answer "what term is
// the entry at index N?" while holding no entries: a log of ten million records that
// changed leader four times is five of these.
struct Epoch {
  Term term = kNoTerm;
  Index start = 0;  // first index produced in this term

  friend bool operator==(const Epoch& a, const Epoch& b) noexcept {
    return a.term == b.term && a.start == b.start;
  }
};

const char* to_string(Role role) noexcept;

// The two facts Raft's safety proof requires to survive a crash (§13, and rule 4 in
// CLAUDE.md). A node that votes in term T, crashes, and comes back not remembering the
// vote can vote a second time in T and elect two leaders — I6, violated, by a node that
// did nothing wrong except forget.
//
// Not tunable, ever. `acks` is a knob about the user's data; this is not.
struct HardState {
  Term term = 0;
  NodeId voted_for = kNoNode;

  friend bool operator==(const HardState& a, const HardState& b) noexcept {
    return a.term == b.term && a.voted_for == b.voted_for;
  }
  friend bool operator!=(const HardState& a, const HardState& b) noexcept { return !(a == b); }
};

enum class MessageType : base::u8 {
  kRequestVote = 1,
  kRequestVoteResponse = 2,
  kAppendEntries = 3,
  kAppendEntriesResponse = 4,
};

const char* to_string(MessageType type) noexcept;

// One Raft RPC, request or response, in one flat struct.
//
// Deliberately not a variant: every message is under 64 bytes, the union of all four
// field sets is small, and a flat POD encodes to a fixed-width frame with no length
// prefixes to get wrong. The cost is that some fields are meaningless for some types,
// which the comments below pin down.
struct Message {
  MessageType type = MessageType::kRequestVote;
  NodeId from = kNoNode;
  NodeId to = kNoNode;
  Term term = 0;

  // RequestVote — the candidate's log end, for the up-to-date check (Raft §5.4.1).
  // AppendEntries — the entry immediately *preceding* the attached one, for log matching
  //   (Raft §5.3). `kNoIndex` means the attached entry is the first in the log.
  // AppendEntriesResponse — the responder's log end, so a leader can set match/next
  //   without a second round trip.
  Index log_index = 0;
  Term log_term = 0;

  // AppendEntries only: the leader's commit index, which the follower adopts clamped to
  // its own log end. This is Kafka's high watermark (§12.1) — a consumer may not read at
  // or past it, which is what makes a truncated entry impossible to have read.
  Index commit_index = 0;

  // **At most one entry per AppendEntries, and it is a whole record batch.**
  //
  // §16.2: "a batch is exactly one Raft entry — this is what amortizes replication cost".
  // So there is no entry array, no per-entry length prefixes, and no ambiguity about how
  // many terms a single message spans: the batch header already carries `base_offset`
  // (the index) and `leader_epoch` (the term), which makes an entry self-describing on
  // the wire and on disk.
  //
  // `kNoIndex` here means the message is a heartbeat and carries nothing.
  //
  // **`entry_end` is not `entry_index + 1`**, and that is the whole reason it exists.
  // Indices here are storage offsets, one per *record*, while an entry is a whole batch —
  // so a batch of four records at offset 12 ends at 16, not 13. §12.2 already says offsets
  // are monotonic but not dense; this is where the receiver finds out by how much.
  //
  // The leader's *driver* fills it in, because only storage knows where a batch ends. The
  // node names the index; the driver supplies the extent.
  Index entry_index = kNoIndex;
  Index entry_end = 0;
  Term entry_term = 0;

  // Responses only: vote granted, or append accepted.
  bool granted = false;

  [[nodiscard]] bool carries_entry() const noexcept { return entry_index != kNoIndex; }
};

struct Config {
  NodeId id = 0;
  std::vector<NodeId> peers;  // every other member; this node is not in here

  // Timeouts count **ticks, not nanoseconds**, and that is the point (§17). A state
  // machine that cannot read a clock cannot be broken by one that jumps, so the clock
  // fault in the simulator has no path to Raft's correctness at all. The driver decides
  // what a tick is worth and is the only thing that owns a timer.
  //
  // The jitter is not decoration. Every node timing out at the same instant is the
  // classic split-vote loop: all of them campaign, all of them reject each other, and
  // the term climbs with nobody winning.
  base::u32 election_timeout_ticks = 15;
  base::u32 election_timeout_jitter_ticks = 15;
  base::u32 heartbeat_timeout_ticks = 5;

  [[nodiscard]] std::size_t cluster_size() const noexcept { return peers.size() + 1; }
  [[nodiscard]] std::size_t quorum() const noexcept { return cluster_size() / 2 + 1; }
};

// Everything the state machine wants the outside world to do, handed over in one batch.
//
// **The order is the contract, and it is the whole of §13:** if `persist_hard_state` is
// set, `hard_state` must be on the platter — written *and* fsynced — before a single
// message in `messages` leaves the node. Bundling them in one struct is what makes that
// rule enforceable in one place instead of being a thing four call sites must remember.
struct Ready {
  bool persist_hard_state = false;
  HardState hard_state;
  std::vector<Message> messages;

  // ---- Replication (week 5). The driver owns the bytes; the node owns the decisions. ----
  //
  // `raft::Node` never holds a record. It holds only what it needs to *decide*: the log's
  // end, and a compact map of which term produced which range of indices. The entries
  // themselves live in `storage::Log` and nowhere else, which is the whole of §12 — the
  // user's log *is* the Raft log, so a second copy inside the consensus layer would
  // reintroduce exactly the double write that decision exists to avoid.
  //
  // The division of labour that falls out of it:
  //   * the node says *which* index to send to a peer; the driver reads that batch from
  //     storage and attaches it,
  //   * the node says whether an arriving entry is acceptable; the driver, which is
  //     holding the bytes it just decoded, writes it,
  //   * the node says what to throw away; the driver truncates.

  // Drop everything from this index onward before appending — a follower whose tail
  // diverged from the leader's (Raft §5.3). `kNoIndex` when there is nothing to drop.
  // **Ordered before the append**, and the driver must not reorder them.
  Index truncate_from = kNoIndex;

  // The entry attached to the message just stepped is accepted and should be written.
  // Only ever set while handling an AppendEntries that carried one.
  bool append_entry = false;

  // Entries below this index are committed and may be served to consumers (I5). Only
  // meaningful when it differs from what the driver last saw; it never moves backwards.
  Index commit_index = 0;

  [[nodiscard]] bool empty() const noexcept {
    return !persist_hard_state && messages.empty() && !append_entry &&
           truncate_from == kNoIndex;
  }
};

}  // namespace raft
