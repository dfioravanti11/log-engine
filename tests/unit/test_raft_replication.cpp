#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "io/seeded_random.h"
#include "raft/node.h"

// Replication, tested with no storage, no disk, and no event loop.
//
// `raft::Node` holds no entries — only the log's end and a map of which term produced
// which range (§12.3). So a "log" in these tests is a `vector<Term>` indexed by offset,
// and a "driver" is the thirty lines of `Replica` below that carry out a `Ready` in the
// order `Ready` states it. That is the same contract the simulator's driver implements
// against `storage::Log`, which is what makes these tests worth writing: if the state
// machine's idea of the log and the driver's diverge, it shows up here first, in a test
// that runs in a millisecond and names the exact index.
namespace {

using raft::Index;
using raft::Message;
using raft::MessageType;
using raft::Node;
using raft::NodeId;
using raft::Term;

raft::Config config_for(NodeId id, base::u32 size) {
  raft::Config config;
  config.id = id;
  for (base::u32 peer = 0; peer < size; ++peer) {
    if (peer != id) config.peers.push_back(peer);
  }
  return config;
}

// One node plus the log its driver would own.
class Replica {
 public:
  Replica(raft::Config config, io::Random& rng)
      : node_(std::move(config), rng, raft::HardState{}) {}

  Node& node() { return node_; }
  [[nodiscard]] const std::vector<Term>& log() const { return log_; }
  [[nodiscard]] Index committed() const { return committed_; }

  // Leader side: append locally at the next offset, stamped with the current term, then
  // tell the node. `storage::Log` is the offset authority in the real system, which is
  // why the index flows *into* the node rather than out of it.
  Index propose() {
    const Index index = log_.size();
    log_.push_back(node_.term());
    // One record per entry in these tests, so an entry starting at `index` ends at
    // `index + 1`. The simulator's batches hold several, which is exactly the mismatch
    // `entry_end` exists to carry.
    node_.log_appended(index, index + 1, node_.term());
    return index;
  }

  void restore(std::vector<Term> log) {
    log_ = std::move(log);
    std::vector<raft::Epoch> epochs;
    for (Index i = 0; i < log_.size(); ++i) {
      if (epochs.empty() || epochs.back().term != log_[i]) {
        epochs.push_back(raft::Epoch{log_[i], i});
      }
    }
    node_.restore_log(log_.size(), epochs);
  }

  // Carry out one Ready. `stepped` is the message just delivered, if any — it is where
  // the entry's bytes would come from in the real driver.
  std::vector<Message> drain(const Message* stepped) {
    raft::Ready ready = node_.take_ready();
    if (ready.persist_hard_state) node_.hard_state_persisted();

    // Order is the contract: truncate, then append.
    if (ready.truncate_from != raft::kNoIndex) {
      EXPECT_LE(ready.truncate_from, log_.size());
      log_.resize(std::min<std::size_t>(ready.truncate_from, log_.size()));
    }
    if (ready.append_entry) {
      EXPECT_NE(stepped, nullptr) << "an append was ordered with no message to take it from";
      if (stepped != nullptr) {
        EXPECT_EQ(stepped->entry_index, log_.size())
            << "the node asked for an append that would leave a hole in the log";
        log_.push_back(stepped->entry_term);
      }
    }
    EXPECT_GE(ready.commit_index, committed_) << "the commit index went backwards";
    committed_ = ready.commit_index;
    EXPECT_LE(committed_, log_.size()) << "committed past the end of the log (I5)";
    return std::move(ready.messages);
  }

 private:
  Node node_;
  std::vector<Term> log_;
  Index committed_ = 0;
};

class Cluster {
 public:
  explicit Cluster(base::u32 size, base::u64 seed = 1) : rng_(seed) {
    for (base::u32 id = 0; id < size; ++id) {
      replicas_.push_back(std::make_unique<Replica>(config_for(id, size), rng_));
    }
  }

  Replica& at(base::u32 id) { return *replicas_[id]; }
  Node& node(base::u32 id) { return replicas_[id]->node(); }
  [[nodiscard]] std::size_t size() const { return replicas_.size(); }

  void block(std::function<bool(const Message&)> predicate) {
    blocked_ = std::move(predicate);
  }

  void collect(base::u32 id, const Message* stepped) {
    for (Message& out : replicas_[id]->drain(stepped)) queue_.push_back(out);
  }

  void tick_all() {
    for (base::u32 id = 0; id < replicas_.size(); ++id) {
      replicas_[id]->node().tick();
      collect(id, nullptr);
    }
  }

  // Enough ticks for the leader's heartbeat to fire (every 5) but comfortably fewer than
  // an election timeout (15 at minimum), so this advances the conversation without
  // costing a leader. Each heartbeat resets the followers' timers, so this stays safe.
  bool run_ticks(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      tick_all();
      if (!settle()) return false;
    }
    return true;
  }

  bool settle(std::size_t max_rounds = 400) {
    for (std::size_t round = 0; round < max_rounds; ++round) {
      if (queue_.empty()) return true;
      std::vector<Message> batch;
      batch.swap(queue_);
      for (const Message& message : batch) {
        if (message.to >= replicas_.size()) continue;
        if (blocked_ && blocked_(message)) continue;
        replicas_[message.to]->node().step(message);
        collect(message.to, &message);
      }
    }
    return false;
  }

  // Elects `id` and returns once it is leading and everyone knows.
  void elect(base::u32 id) {
    replicas_[id]->node().campaign();
    collect(id, nullptr);
    ASSERT_TRUE(settle());
    ASSERT_TRUE(replicas_[id]->node().is_leader());
  }

  [[nodiscard]] std::size_t leader_count() const {
    std::size_t count = 0;
    for (const auto& replica : replicas_) {
      if (replica->node().is_leader()) ++count;
    }
    return count;
  }

 private:
  io::SeededRandom rng_;
  std::vector<std::unique_ptr<Replica>> replicas_;
  std::vector<Message> queue_;
  std::function<bool(const Message&)> blocked_;
};

TEST(RaftLogMetadata, TermAtFindsTheProducingEpoch) {
  io::SeededRandom rng(1);
  Node node(config_for(0, 3), rng, raft::HardState{});
  // Indices 0-2 from term 1, 3-3 from term 4, 4-6 from term 9.
  node.restore_log(7, {raft::Epoch{1, 0}, raft::Epoch{4, 3}, raft::Epoch{9, 4}});

  EXPECT_EQ(node.term_at(0), 1u);
  EXPECT_EQ(node.term_at(2), 1u);
  EXPECT_EQ(node.term_at(3), 4u);
  EXPECT_EQ(node.term_at(4), 9u);
  EXPECT_EQ(node.term_at(6), 9u);
  EXPECT_EQ(node.term_at(7), raft::kNoTerm) << "one past the end is not an entry";
  EXPECT_EQ(node.term_at(99), raft::kNoTerm);
  EXPECT_EQ(node.log_end(), 7u);
  EXPECT_EQ(node.last_term(), 9u);
}

TEST(RaftLogMetadata, AnEmptyLogHasNoTerms) {
  io::SeededRandom rng(1);
  Node node(config_for(0, 3), rng, raft::HardState{});
  EXPECT_EQ(node.log_end(), 0u);
  EXPECT_EQ(node.last_term(), raft::kNoTerm);
  EXPECT_EQ(node.term_at(0), raft::kNoTerm);
}

TEST(RaftLogMetadata, ConsecutiveEntriesFromOneLeaderShareOneEpoch) {
  io::SeededRandom rng(1);
  Node node(config_for(0, 1), rng, raft::HardState{});
  node.campaign();  // term 1, and a single-node cluster wins immediately
  for (Index i = 0; i < 100; ++i) node.log_appended(i, i + 1, node.term());

  EXPECT_EQ(node.log_end(), 100u);
  EXPECT_EQ(node.term_at(0), 1u);
  EXPECT_EQ(node.term_at(99), 1u);
  // The point of the epoch map: a hundred entries, one epoch. A log of ten million that
  // changed leader four times is five entries of metadata, not ten million.
}

TEST(RaftReplication, ALeaderReplicatesToEveryFollower) {
  Cluster cluster(3);
  cluster.elect(0);

  for (int i = 0; i < 5; ++i) {
    cluster.at(0).propose();
    cluster.collect(0, nullptr);
  }
  ASSERT_TRUE(cluster.settle());

  ASSERT_EQ(cluster.at(0).log().size(), 5u);
  EXPECT_EQ(cluster.at(1).log(), cluster.at(0).log());
  EXPECT_EQ(cluster.at(2).log(), cluster.at(0).log());
}

TEST(RaftReplication, AnEntryCommitsOnceAMajorityHoldsIt) {
  Cluster cluster(3);
  cluster.elect(0);

  for (int i = 0; i < 3; ++i) {
    cluster.at(0).propose();
    cluster.collect(0, nullptr);
  }
  ASSERT_TRUE(cluster.settle());

  EXPECT_EQ(cluster.node(0).commit_index(), 3u);
  // Followers learn the commit index from the *next* AppendEntries, not from their own
  // ack — so a follower's view of what is readable always trails the leader's by one
  // round trip. That gap is not a bug; it is why a consumer reading from a follower can
  // be behind, and it is the same gap §12.1 says to benchmark.
  ASSERT_TRUE(cluster.run_ticks(6));
  EXPECT_EQ(cluster.at(1).committed(), 3u);
  EXPECT_EQ(cluster.at(2).committed(), 3u);
}

TEST(RaftReplication, NothingCommitsWithoutAMajority) {
  Cluster cluster(3);
  cluster.elect(0);
  // Both followers go silent. The leader can still append locally — and must not commit.
  cluster.block([](const Message& message) {
    return message.type == MessageType::kAppendEntriesResponse;
  });

  for (int i = 0; i < 4; ++i) {
    cluster.at(0).propose();
    cluster.collect(0, nullptr);
  }
  ASSERT_TRUE(cluster.settle());

  EXPECT_EQ(cluster.at(0).log().size(), 4u);
  EXPECT_EQ(cluster.node(0).commit_index(), 0u)
      << "a leader that commits without hearing from a majority has invented durability";
}

TEST(RaftReplication, AMinorityIsEnoughToBlockButNotToStop) {
  Cluster cluster(3);
  cluster.elect(0);
  // One follower silent. Two of three is still a majority, so progress continues.
  cluster.block([](const Message& message) { return message.from == 2 || message.to == 2; });

  for (int i = 0; i < 3; ++i) {
    cluster.at(0).propose();
    cluster.collect(0, nullptr);
  }
  ASSERT_TRUE(cluster.settle());

  EXPECT_EQ(cluster.node(0).commit_index(), 3u);
  EXPECT_EQ(cluster.at(1).log().size(), 3u);
  EXPECT_TRUE(cluster.at(2).log().empty());
}

TEST(RaftReplication, ASlowFollowerCatchesUpOneEntryPerRoundTrip) {
  Cluster cluster(3);
  cluster.elect(0);
  cluster.block([](const Message& message) { return message.from == 2 || message.to == 2; });

  for (int i = 0; i < 6; ++i) {
    cluster.at(0).propose();
    cluster.collect(0, nullptr);
  }
  ASSERT_TRUE(cluster.settle());
  ASSERT_TRUE(cluster.at(2).log().empty());

  // It comes back. One heartbeat restarts the conversation, and from there the response
  // loop feeds it the whole backlog — one entry per round trip, not one per heartbeat.
  // That distinction is the difference between a follower rejoining in milliseconds and
  // rejoining in (backlog × heartbeat interval).
  cluster.block(nullptr);
  ASSERT_TRUE(cluster.run_ticks(6));

  EXPECT_EQ(cluster.at(2).log(), cluster.at(0).log());
}

Message append_entries(NodeId from, NodeId to, Term term, Index prev, Term prev_term,
                       Index entry, Term entry_term, Index commit = 0) {
  Message message;
  message.type = MessageType::kAppendEntries;
  message.from = from;
  message.to = to;
  message.term = term;
  message.log_index = prev;
  message.log_term = prev_term;
  message.entry_index = entry;
  message.entry_end = entry + 1;  // one record per entry here
  message.entry_term = entry_term;
  message.commit_index = commit;
  return message;
}

// Raft §5.3. The follower's tail came from a leader that lost; the new leader's entries
// replace it, and the follower must throw its own away rather than splice around them.
//
// Driven directly rather than through an election, deliberately. An earlier version of
// this test set up three nodes, ran an election, and skipped itself if the node it wanted
// did not happen to win — which is a test that can go quiet the first time a timing change
// shifts the outcome, and this project has been bitten by silent checks enough times.
TEST(RaftReplication, AFollowerWithADivergentTailTruncatesIt) {
  io::SeededRandom rng(1);
  Replica follower(config_for(1, 3), rng);
  follower.restore({7, 7, 7});  // three entries from a term that never committed

  // A leader in term 9 claims index 0. `kNoIndex` as the predecessor means "this is the
  // first entry in the log", which always matches — so the follower's three entries are
  // all in the way and all have to go.
  const Message append = append_entries(0, 1, 9, raft::kNoIndex, raft::kNoTerm, 0, 9);
  follower.node().step(append);
  const std::vector<Message> out = follower.drain(&append);

  ASSERT_EQ(follower.log().size(), 1u) << "the divergent tail survived the append";
  EXPECT_EQ(follower.log()[0], 9u);
  EXPECT_EQ(follower.node().log_end(), 1u);
  EXPECT_EQ(follower.node().term_at(0), 9u);
  EXPECT_EQ(follower.node().last_term(), 9u);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].granted);
  EXPECT_EQ(out[0].log_index, 1u) << "the response must report the new log end";
}

// The other half, and the one that costs throughput if it is wrong. A leader retries
// whenever a response goes missing, so duplicate AppendEntries are routine — not an edge
// case. Truncating on every one of them would rewrite the tail of the log to put back
// bytes that were already correct.
TEST(RaftReplication, ADuplicateAppendDoesNotRewriteTheTail) {
  io::SeededRandom rng(1);
  Node follower(config_for(1, 3), rng, raft::HardState{});

  const Message first = append_entries(0, 1, 4, raft::kNoIndex, raft::kNoTerm, 0, 4);
  follower.step(first);
  raft::Ready ready = follower.take_ready();
  follower.hard_state_persisted();
  ASSERT_TRUE(ready.append_entry);
  ASSERT_EQ(ready.truncate_from, raft::kNoIndex);
  follower.log_appended(0, 1, 4);  // the driver did as it was told

  // The identical message arrives again.
  follower.step(first);
  raft::Ready again = follower.take_ready();

  EXPECT_EQ(again.truncate_from, raft::kNoIndex)
      << "a retry truncated a tail that already held exactly the right entry";
  EXPECT_FALSE(again.append_entry) << "a retry re-appended an entry already present";
  EXPECT_EQ(follower.log_end(), 1u);
  ASSERT_EQ(again.messages.size(), 1u);
  EXPECT_TRUE(again.messages[0].granted) << "a duplicate is still an accept, not a reject";
}

// A mismatch at the predecessor means everything from there is suspect. The follower
// refuses with a hint that is **always a batch boundary**, because the leader has no idea
// where batches begin and `index - 1` would land in the middle of one.
TEST(RaftReplication, AMismatchedPredecessorIsRejectedWithABoundaryHint) {
  io::SeededRandom rng(1);
  Replica follower(config_for(1, 3), rng);
  follower.restore({2, 2, 2});  // one epoch, term 2, starting at index 0

  // The leader thinks index 1 was produced in term 5. This follower says term 2, and
  // everything in that epoch is therefore suspect — so it sends the leader back to where
  // the epoch began rather than one index at a time (the conflicting-term optimization).
  const Message append = append_entries(0, 1, 9, 1, 5, 2, 9);
  follower.node().step(append);
  const std::vector<Message> out = follower.drain(&append);

  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].granted);
  EXPECT_EQ(out[0].log_index, 0u) << "the hint must be the start of the disputed epoch";
  EXPECT_EQ(follower.log().size(), 3u) << "a rejected append must not modify the log";
}

TEST(RaftReplication, RunningPastTheEndOfTheFollowersLogIsHintedWithItsEnd) {
  io::SeededRandom rng(1);
  Replica follower(config_for(1, 3), rng);
  follower.restore({2, 2, 2});

  // The leader is far ahead and names a predecessor this follower has never seen.
  const Message append = append_entries(0, 1, 9, 40, 9, 41, 9);
  follower.node().step(append);
  const std::vector<Message> out = follower.drain(&append);

  ASSERT_EQ(out.size(), 1u);
  EXPECT_FALSE(out[0].granted);
  EXPECT_EQ(out[0].log_index, 3u) << "nothing is disputed, so resume from our end";
}

// **A follower reports what matched, not how long its log is**, and the difference is a
// safety bug rather than an accounting one. A follower can hold a longer log than the
// leader — a divergent tail from a term that lost — and a heartbeat agreeing about the
// entry at `prev` says nothing about the entries after it. Reporting the full length
// would let `match_` run past the leader's own log and count entries it has never seen
// toward committing entries it has.
TEST(RaftReplication, AHeartbeatConfirmsOnlyUpToThePredecessor) {
  io::SeededRandom rng(1);
  Replica follower(config_for(1, 3), rng);
  follower.restore({4, 4, 4, 4, 4});  // five entries, all term 4

  // A leader in term 4 with a *shorter* log heartbeats about index 1.
  Message heartbeat = append_entries(0, 1, 4, 1, 4, 0, 0);
  heartbeat.entry_index = raft::kNoIndex;
  heartbeat.entry_end = 0;

  follower.node().step(heartbeat);
  const std::vector<Message> out = follower.drain(&heartbeat);

  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].granted);
  EXPECT_EQ(out[0].log_index, 2u)
      << "confirmed through index 1, not all five entries this follower happens to hold";
  EXPECT_EQ(follower.log().size(), 5u) << "a heartbeat must not truncate anything";
}

// **Raft §5.4.2**, and the subtlest rule in the paper. An entry from an earlier term that
// a majority happens to hold is *not* committable on that count alone: a future leader is
// still entitled to replace it, so committing it would promise something that can be
// taken back. It commits only when an entry from the leader's own term commits with it.
TEST(RaftReplication, AnEntryFromAnEarlierTermIsNotCommittedOnCountAlone) {
  Cluster cluster(3);

  // The leader-to-be holds one entry produced in term 1.
  cluster.at(0).restore({1});
  // Drive it to a later term and let it win.
  for (int i = 0; i < 4; ++i) cluster.node(0).campaign();
  cluster.collect(0, nullptr);
  ASSERT_TRUE(cluster.settle());
  ASSERT_TRUE(cluster.node(0).is_leader());
  ASSERT_GT(cluster.node(0).term(), 1u);

  // Both followers accept the old entry. That is a majority holding index 0.
  ASSERT_EQ(cluster.at(1).log().size(), 1u);
  ASSERT_EQ(cluster.at(2).log().size(), 1u);
  EXPECT_EQ(cluster.node(0).commit_index(), 0u)
      << "committed an entry from an earlier term on the majority count alone";

  // One entry from the leader's own term, and both commit together.
  cluster.at(0).propose();
  cluster.collect(0, nullptr);
  ASSERT_TRUE(cluster.settle());
  EXPECT_EQ(cluster.node(0).commit_index(), 2u);
}

// **I4 — a committed entry is never overwritten.** The §5.4.1 up-to-date check is what
// guarantees it: a node missing a committed entry cannot win an election, so no leader
// ever exists that would want to replace one.
TEST(RaftReplication, ACommittedEntrySurvivesEveryLeaderChange) {
  for (base::u64 seed = 1; seed <= 30; ++seed) {
    Cluster cluster(3, seed);
    cluster.elect(0);

    for (int i = 0; i < 4; ++i) {
      cluster.at(0).propose();
      cluster.collect(0, nullptr);
    }
    ASSERT_TRUE(cluster.settle()) << "seed " << seed;

    const Index committed = cluster.node(0).commit_index();
    ASSERT_GT(committed, 0u) << "seed " << seed;
    const std::vector<Term> prefix(cluster.at(0).log().begin(),
                                   cluster.at(0).log().begin() +
                                       static_cast<std::ptrdiff_t>(committed));

    // Hand leadership around and keep proposing.
    for (base::u32 next : {1u, 2u, 0u}) {
      cluster.node(next).campaign();
      cluster.collect(next, nullptr);
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;
      if (!cluster.node(next).is_leader()) continue;

      cluster.at(next).propose();
      cluster.collect(next, nullptr);
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;

      for (base::u32 id = 0; id < cluster.size(); ++id) {
        const std::vector<Term>& log = cluster.at(id).log();
        if (log.size() < prefix.size()) continue;  // still catching up
        for (std::size_t i = 0; i < prefix.size(); ++i) {
          ASSERT_EQ(log[i], prefix[i])
              << "seed " << seed << ": node " << id << " overwrote committed index " << i;
        }
      }
    }
  }
}

// **I2 — offsets are monotonic**, and its sharper form for a replicated log: two nodes
// never hold different entries at the same index (the Log Matching Property, §5.3).
TEST(RaftReplication, NoTwoNodesEverDisagreeAtTheSameIndex) {
  for (base::u64 seed = 1; seed <= 30; ++seed) {
    Cluster cluster(3, seed);

    for (int round = 0; round < 12; ++round) {
      const base::u32 candidate = static_cast<base::u32>(round % 3);
      cluster.node(candidate).campaign();
      cluster.collect(candidate, nullptr);
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;

      if (cluster.node(candidate).is_leader()) {
        cluster.at(candidate).propose();
        cluster.collect(candidate, nullptr);
        ASSERT_TRUE(cluster.settle()) << "seed " << seed;
      }

      for (base::u32 a = 0; a < cluster.size(); ++a) {
        for (base::u32 b = a + 1; b < cluster.size(); ++b) {
          const std::vector<Term>& x = cluster.at(a).log();
          const std::vector<Term>& y = cluster.at(b).log();
          const std::size_t common = std::min(x.size(), y.size());
          for (std::size_t i = 0; i < common; ++i) {
            // Log matching is a statement about *committed* prefixes; an uncommitted tail
            // may legitimately differ between a deposed leader and the new one. Compare
            // only up to what both have agreed on.
            const Index safe = std::min(cluster.at(a).committed(), cluster.at(b).committed());
            if (i >= safe) break;
            ASSERT_EQ(x[i], y[i]) << "seed " << seed << ": nodes " << a << " and " << b
                                  << " disagree at committed index " << i;
          }
        }
      }
    }
  }
}

TEST(RaftReplication, TheCommitIndexNeverMovesBackwards) {
  // Replica::drain() asserts this on every single Ready, so the test body only has to
  // make the cluster work hard enough to be interesting.
  for (base::u64 seed = 1; seed <= 20; ++seed) {
    Cluster cluster(3, seed);
    for (int round = 0; round < 10; ++round) {
      cluster.node(static_cast<base::u32>(round % 3)).campaign();
      cluster.collect(static_cast<base::u32>(round % 3), nullptr);
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;
      for (base::u32 id = 0; id < cluster.size(); ++id) {
        if (cluster.node(id).is_leader()) {
          cluster.at(id).propose();
          cluster.collect(id, nullptr);
        }
      }
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;
    }
  }
}

TEST(RaftReplication, ASingleNodeClusterCommitsItsOwnAppendsImmediately) {
  Cluster cluster(1);
  cluster.elect(0);

  cluster.at(0).propose();
  cluster.collect(0, nullptr);
  EXPECT_EQ(cluster.node(0).commit_index(), 1u);
}

}  // namespace
