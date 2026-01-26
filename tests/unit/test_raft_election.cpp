#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "io/seeded_random.h"
#include "raft/node.h"

// Leader election, tested with **no infrastructure at all**.
//
// There is no event loop in this file, no simulated disk, no network, and no notion of
// time beyond "somebody called tick()". That is the payoff of making `raft::Node` a pure
// state machine (see the class comment): a three-node election is three objects and a
// message queue, the whole file runs in milliseconds, and when the simulator later
// reports a failure under some seed, these tests have already ruled out the algorithm
// itself — which is the difference between debugging one layer and debugging four.
namespace {

using raft::HardState;
using raft::Message;
using raft::MessageType;
using raft::Node;
using raft::NodeId;
using raft::Role;
using raft::Term;

raft::Config config_for(NodeId id, base::u32 size) {
  raft::Config config;
  config.id = id;
  for (base::u32 peer = 0; peer < size; ++peer) {
    if (peer != id) config.peers.push_back(peer);
  }
  return config;
}

Message vote_request(NodeId from, NodeId to, Term term, raft::Index index = 0,
                     Term log_term = 0) {
  Message message;
  message.type = MessageType::kRequestVote;
  message.from = from;
  message.to = to;
  message.term = term;
  message.log_index = index;
  message.log_term = log_term;
  return message;
}

// A cluster of state machines and a queue between them. Messages move in rounds; a round
// delivers everything currently queued and collects whatever that produced.
class Cluster {
 public:
  using Tune = std::function<void(raft::Config&)>;

  explicit Cluster(base::u32 size, base::u64 seed = 1, const Tune& tune = nullptr)
      : rng_(seed) {
    for (base::u32 id = 0; id < size; ++id) {
      raft::Config config = config_for(id, size);
      if (tune) tune(config);
      nodes_.push_back(std::make_unique<Node>(std::move(config), rng_, HardState{}));
    }
  }

  Node& node(base::u32 id) { return *nodes_[id]; }
  [[nodiscard]] std::size_t size() const { return nodes_.size(); }

  void tick_all() {
    for (auto& node : nodes_) node->tick();
  }

  // Runs until nothing is in flight. Returns false if it never quiesced, which in these
  // tests means something is generating messages forever.
  bool settle(std::size_t max_rounds = 200) {
    for (std::size_t round = 0; round < max_rounds; ++round) {
      collect();
      if (queue_.empty()) return true;

      std::vector<Message> batch;
      batch.swap(queue_);
      for (const Message& message : batch) {
        if (message.to >= nodes_.size()) continue;
        if (blocked_ && blocked_(message)) continue;
        nodes_[message.to]->step(message);
      }
    }
    return false;
  }

  void block(std::function<bool(const Message&)> predicate) {
    blocked_ = std::move(predicate);
  }

  [[nodiscard]] std::size_t leader_count() const {
    std::size_t count = 0;
    for (const auto& node : nodes_) {
      if (node->is_leader()) ++count;
    }
    return count;
  }

  [[nodiscard]] Term highest_term() const {
    Term highest = 0;
    for (const auto& node : nodes_) highest = std::max(highest, node->term());
    return highest;
  }

  // **I6 — at most one leader per term**, checked the way the invariant is actually
  // stated. Counting leaders at an instant is a weaker claim: two leaders in *different*
  // terms is legal and routine, and a check that flagged it would be wrong.
  [[nodiscard]] bool one_leader_per_term() {
    for (const auto& node : nodes_) {
      if (!node->is_leader()) continue;
      auto [it, inserted] = leader_by_term_.emplace(node->term(), node->id());
      if (!inserted && it->second != node->id()) return false;
    }
    return true;
  }

 private:
  // Stands in for a driver whose disk works: persist, acknowledge, then send. The
  // acknowledgement is not optional bookkeeping — the node keeps owing the write until
  // it hears the fsync returned, which is what `PersistFailureTest` below relies on.
  void collect() {
    for (auto& node : nodes_) {
      raft::Ready ready = node->take_ready();
      if (ready.persist_hard_state) node->hard_state_persisted();
      for (const Message& message : ready.messages) queue_.push_back(message);
    }
  }

  io::SeededRandom rng_;
  std::vector<std::unique_ptr<Node>> nodes_;
  std::vector<Message> queue_;
  std::function<bool(const Message&)> blocked_;
  std::map<Term, NodeId> leader_by_term_;
};

TEST(RaftElection, ASingleNodeClusterIsItsOwnQuorum) {
  Cluster cluster(1);
  cluster.node(0).campaign();

  EXPECT_TRUE(cluster.node(0).is_leader());
  EXPECT_EQ(cluster.node(0).term(), 1u);
  EXPECT_EQ(cluster.node(0).leader(), 0u);
}

TEST(RaftElection, ACandidateWithAMajorityBecomesLeaderAndTheRestFollow) {
  Cluster cluster(3);
  cluster.node(0).campaign();
  ASSERT_TRUE(cluster.settle());

  EXPECT_TRUE(cluster.node(0).is_leader());
  EXPECT_EQ(cluster.node(0).term(), 1u);
  for (base::u32 id : {1u, 2u}) {
    EXPECT_EQ(cluster.node(id).role(), Role::kFollower);
    EXPECT_EQ(cluster.node(id).term(), 1u);
    // The follower learns who won from the heartbeat, not from the vote it cast — a
    // candidate is not a leader until it has counted the votes.
    EXPECT_EQ(cluster.node(id).leader(), 0u);
  }
}

TEST(RaftElection, ACandidateThatCannotReachAMajorityStaysACandidate) {
  Cluster cluster(3);
  // Node 0 can send, but nothing comes back. It has its own vote and no other.
  cluster.block([](const Message& message) {
    return message.type == MessageType::kRequestVoteResponse;
  });

  cluster.node(0).campaign();
  ASSERT_TRUE(cluster.settle());

  EXPECT_EQ(cluster.node(0).role(), Role::kCandidate);
  EXPECT_EQ(cluster.node(0).votes(), 1u);
  EXPECT_EQ(cluster.leader_count(), 0u);
}

TEST(RaftElection, ANodeVotesAtMostOncePerTerm) {
  Cluster cluster(3);
  Node& voter = cluster.node(2);

  voter.step(vote_request(1, 2, 5));
  raft::Ready first = voter.take_ready();
  ASSERT_EQ(first.messages.size(), 1u);
  EXPECT_TRUE(first.messages[0].granted);
  EXPECT_EQ(voter.voted_for(), 1u);

  voter.step(vote_request(0, 2, 5));
  raft::Ready second = voter.take_ready();
  ASSERT_EQ(second.messages.size(), 1u);
  EXPECT_FALSE(second.messages[0].granted) << "two votes in one term is how I6 breaks";
  EXPECT_EQ(voter.voted_for(), 1u);
}

TEST(RaftElection, TheSameCandidateAskingTwiceIsGrantedTwice) {
  Cluster cluster(3);
  Node& voter = cluster.node(2);

  voter.step(vote_request(1, 2, 5));
  ASSERT_TRUE(voter.take_ready().messages[0].granted);

  // A lost response makes the candidate retry. Refusing the retry would cost an election
  // for no safety benefit — the vote is already committed to this candidate.
  voter.step(vote_request(1, 2, 5));
  raft::Ready again = voter.take_ready();
  ASSERT_EQ(again.messages.size(), 1u);
  EXPECT_TRUE(again.messages[0].granted);
}

TEST(RaftElection, AStaleCandidateIsAnsweredWithTheCurrentTerm) {
  Cluster cluster(3);
  Node& voter = cluster.node(2);
  voter.step(vote_request(1, 2, 9));
  (void)voter.take_ready();

  voter.step(vote_request(0, 2, 4));
  raft::Ready reply = voter.take_ready();
  ASSERT_EQ(reply.messages.size(), 1u);
  EXPECT_FALSE(reply.messages[0].granted);
  // Answering rather than dropping is what makes the stale node step down now instead of
  // after its own timeout. A rejoining node with an old term is the common case.
  EXPECT_EQ(reply.messages[0].term, 9u);
}

// Raft §5.4.1. This is the check that makes "the leader holds every committed entry" a
// theorem rather than a hope, and week 5 depends on it entirely.
TEST(RaftElection, AVoterRefusesACandidateWhoseLogIsBehind) {
  struct Case {
    raft::Index log_end;  // the candidate's, exclusive
    Term log_term;
    bool expected;
    const char* why;
  };
  // The voter holds ten entries, indices 0..9, all produced in term 3.
  const Case cases[] = {
      {10, 3, true, "identical log"},
      {11, 3, true, "longer in the same term"},
      {4, 4, true, "shorter but in a later term, which wins outright"},
      {9, 3, false, "same term, shorter log"},
      {99, 2, false, "longer, but its last entry is from an older term"},
  };

  for (const Case& test : cases) {
    Cluster cluster(3);
    Node& voter = cluster.node(2);
    voter.restore_log(10, {raft::Epoch{3, 0}});

    voter.step(vote_request(1, 2, 7, test.log_end, test.log_term));
    raft::Ready reply = voter.take_ready();
    ASSERT_EQ(reply.messages.size(), 1u);
    EXPECT_EQ(reply.messages[0].granted, test.expected) << test.why;
  }
}

TEST(RaftElection, ALeaderStepsDownWhenItSeesAHigherTerm) {
  Cluster cluster(3);
  cluster.node(0).campaign();
  ASSERT_TRUE(cluster.settle());
  ASSERT_TRUE(cluster.node(0).is_leader());

  cluster.node(0).step(vote_request(1, 0, 7));
  EXPECT_EQ(cluster.node(0).role(), Role::kFollower);
  EXPECT_EQ(cluster.node(0).term(), 7u);
  // The new term wiped the old vote, and it was spent on the candidate that carried it.
  EXPECT_EQ(cluster.node(0).voted_for(), 1u);
}

TEST(RaftElection, ACandidateStandsDownWhenALeaderOfItsOwnTermSpeaks) {
  Cluster cluster(3);
  cluster.node(0).campaign();  // term 1
  (void)cluster.node(0).take_ready();

  Message heartbeat;
  heartbeat.type = MessageType::kAppendEntries;
  heartbeat.from = 1;
  heartbeat.to = 0;
  heartbeat.term = 1;
  cluster.node(0).step(heartbeat);

  EXPECT_EQ(cluster.node(0).role(), Role::kFollower);
  EXPECT_EQ(cluster.node(0).leader(), 1u);
  EXPECT_EQ(cluster.node(0).voted_for(), 0u) << "an equal term must not clear the vote";
}

TEST(RaftElection, HeartbeatsKeepFollowersFromCampaigning) {
  Cluster cluster(3, 7);
  cluster.node(0).campaign();
  ASSERT_TRUE(cluster.settle());
  ASSERT_TRUE(cluster.node(0).is_leader());
  const Term settled = cluster.node(0).term();

  for (int tick = 0; tick < 400; ++tick) {
    cluster.tick_all();
    ASSERT_TRUE(cluster.settle());
  }

  EXPECT_TRUE(cluster.node(0).is_leader());
  EXPECT_EQ(cluster.highest_term(), settled) << "a stable leader must not cost terms";
  EXPECT_EQ(cluster.leader_count(), 1u);
}

// The headline property, over enough seeds that a lucky jitter draw cannot carry it.
TEST(RaftElection, ExactlyOneLeaderPerTermOverManySeeds) {
  for (base::u64 seed = 1; seed <= 200; ++seed) {
    Cluster cluster(3, seed);

    bool elected = false;
    for (int tick = 0; tick < 400 && !elected; ++tick) {
      cluster.tick_all();
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;
      ASSERT_TRUE(cluster.one_leader_per_term()) << "I6 violated, seed " << seed;
      elected = cluster.leader_count() == 1;
    }
    EXPECT_TRUE(elected) << "no leader after 400 ticks, seed " << seed;
  }
}

TEST(RaftElection, FiveNodesElectALeaderToo) {
  for (base::u64 seed = 1; seed <= 50; ++seed) {
    Cluster cluster(5, seed);
    bool elected = false;
    for (int tick = 0; tick < 400 && !elected; ++tick) {
      cluster.tick_all();
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;
      ASSERT_TRUE(cluster.one_leader_per_term()) << "I6 violated, seed " << seed;
      elected = cluster.leader_count() == 1;
    }
    EXPECT_TRUE(elected) << "seed " << seed;
  }
}

// Why the jitter in Config is not decoration. With every node timing out on the same
// tick, all three campaign, all three vote for themselves, all three reject each other,
// and the term climbs forever with nobody ever winning.
TEST(RaftElection, WithoutJitterLockstepNodesSplitTheVoteForever) {
  Cluster cluster(3, 1, [](raft::Config& config) {
    config.election_timeout_jitter_ticks = 0;
  });

  for (int tick = 0; tick < 300; ++tick) {
    cluster.tick_all();
    ASSERT_TRUE(cluster.settle());
  }

  EXPECT_EQ(cluster.leader_count(), 0u);
  EXPECT_GT(cluster.highest_term(), 15u) << "the term should be climbing with no winner";
}

// **The reason `raft.state` exists, stated as a test.**
//
// A node that loses its vote across a crash is not merely inconvenienced: it hands out a
// second vote in the same term, and two candidates each reach a majority. Two leaders,
// one term, I6 violated — by a node that did nothing wrong except forget. This is the
// bug §13 is about, and it is why the metadata fsync is the one durability knob that is
// not tunable.
TEST(RaftElection, ANodeThatForgetsItsVoteElectsASecondLeaderInTheSameTerm) {
  io::SeededRandom rng(1);
  const Term term = 5;

  Node before(config_for(2, 3), rng, HardState{});
  before.step(vote_request(1, 2, term));
  raft::Ready first = before.take_ready();
  ASSERT_TRUE(first.messages[0].granted);
  ASSERT_TRUE(first.persist_hard_state) << "the vote must be offered for persisting";
  const HardState recorded = first.hard_state;

  // Restart with the vote on disk: the second candidate is refused, as it must be.
  Node remembering(config_for(2, 3), rng, recorded);
  remembering.step(vote_request(0, 2, term));
  EXPECT_FALSE(remembering.take_ready().messages[0].granted);

  // Restart having lost the file: it votes again, for a different candidate, in the same
  // term. Nothing downstream can detect this — the message is perfectly well-formed.
  Node amnesiac(config_for(2, 3), rng, HardState{});
  amnesiac.step(vote_request(0, 2, term));
  EXPECT_TRUE(amnesiac.take_ready().messages[0].granted)
      << "if this ever stops being true, the fsync in the driver has become optional";
}

// The §13 ordering contract, checked as a property of every Ready the node produces: a
// message that depends on a state change is never handed over without the state change
// being offered for persisting in the same batch. The driver's job is then just to do
// them in order.
TEST(RaftElection, AVoteIsNeverAnnouncedWithoutTheStateChangeThatBacksIt) {
  for (base::u64 seed = 1; seed <= 50; ++seed) {
    Cluster cluster(3, seed);

    for (int tick = 0; tick < 200; ++tick) {
      cluster.tick_all();

      for (base::u32 id = 0; id < cluster.size(); ++id) {
        raft::Ready ready = cluster.node(id).take_ready();
        if (ready.persist_hard_state) cluster.node(id).hard_state_persisted();
        bool announces_a_commitment = false;
        for (const Message& message : ready.messages) {
          // Casting a vote, or asking for one — both are promises this node must not
          // forget, and both change term or votedFor.
          if (message.type == MessageType::kRequestVote) announces_a_commitment = true;
          if (message.type == MessageType::kRequestVoteResponse && message.granted) {
            announces_a_commitment = true;
          }
        }
        if (announces_a_commitment) {
          EXPECT_TRUE(ready.persist_hard_state)
              << "node " << id << " would speak before its promise was durable, seed "
              << seed;
        }
        for (const Message& message : ready.messages) {
          if (message.to < cluster.size()) cluster.node(message.to).step(message);
        }
      }
    }
  }
}

// **Bug journal #2**, as a unit test. A follower that can still hear its leader must not
// help a third node depose it — and must not adopt that node's term either, because
// adopting the term *is* the deposing.
TEST(RaftElection, AFollowerWithALiveLeaderWillNotHelpDeposeIt) {
  Cluster cluster(3);
  cluster.node(0).campaign();
  ASSERT_TRUE(cluster.settle());
  ASSERT_EQ(cluster.node(2).leader(), 0u);
  const Term settled = cluster.node(2).term();

  cluster.node(2).step(vote_request(1, 2, settled + 5));
  raft::Ready reply = cluster.node(2).take_ready();

  EXPECT_TRUE(reply.messages.empty()) << "silence, not a rejection carrying a stale term";
  EXPECT_EQ(cluster.node(2).term(), settled) << "the term must not be adopted";
  EXPECT_EQ(cluster.node(2).leader(), 0u);
  EXPECT_EQ(cluster.node(2).lease_refusals(), 1u);
  EXPECT_FALSE(reply.persist_hard_state) << "nothing changed, so nothing needs an fsync";
}

// The other half, and the one that matters more: the rule above delays a failover by at
// most one election timeout — it must never prevent one. A "fix" that kept the cluster
// stable by making it unable to replace a dead leader would be worse than the bug.
TEST(RaftElection, TheLeaseDelaysAFailoverButNeverPreventsOne) {
  for (base::u64 seed = 1; seed <= 20; ++seed) {
    Cluster cluster(3, seed);
    cluster.node(0).campaign();
    ASSERT_TRUE(cluster.settle());
    ASSERT_TRUE(cluster.node(0).is_leader());

    // Node 0 goes silent for good.
    cluster.block([](const Message& message) { return message.from == 0; });

    bool replaced = false;
    for (int tick = 0; tick < 400 && !replaced; ++tick) {
      cluster.tick_all();
      ASSERT_TRUE(cluster.settle()) << "seed " << seed;
      ASSERT_TRUE(cluster.one_leader_per_term()) << "seed " << seed;
      replaced = cluster.node(1).is_leader() || cluster.node(2).is_leader();
    }
    EXPECT_TRUE(replaced) << "seed " << seed << ": the lease rule blocked a real failover";
  }
}

// **A failed write must leave the node still owing one.**
//
// This is the failure mode the sticky debt exists for, and it is a nastier bug than it
// looks because every layer behaves exactly as documented while it happens. The node
// grants a vote, the driver's fsync fails, the driver correctly drops the response — so
// far so good, nobody was told anything. But if taking the Ready had cleared the debt,
// the vote would now live only in memory: when the same candidate retries, the node
// grants again, `voted_for` is *already* that candidate so nothing changed, so nothing
// asks for an fsync, and the grant goes out over a vote that never reached the platter.
// Crash after that and it votes again in the same term.
TEST(RaftElection, AVoteWhoseWriteFailedIsStillOwedOnTheNextPass) {
  io::SeededRandom rng(1);
  Node voter(config_for(2, 3), rng, HardState{});

  voter.step(vote_request(1, 2, 5));
  raft::Ready first = voter.take_ready();
  ASSERT_TRUE(first.persist_hard_state);
  ASSERT_TRUE(first.messages[0].granted);
  // The driver's fsync failed, so it drops the messages and does NOT acknowledge.

  // The same candidate retries. The node grants again — correctly, the vote is already
  // committed to it — and must still be asking for that write.
  voter.step(vote_request(1, 2, 5));
  raft::Ready second = voter.take_ready();
  ASSERT_EQ(second.messages.size(), 1u);
  EXPECT_TRUE(second.messages[0].granted);
  EXPECT_TRUE(second.persist_hard_state)
      << "the node announced a vote it never asked anyone to make durable";
  EXPECT_EQ(second.hard_state.term, 5u);
  EXPECT_EQ(second.hard_state.voted_for, 1u);

  // And once the write does land, the debt clears.
  voter.hard_state_persisted();
  voter.step(vote_request(1, 2, 5));
  EXPECT_FALSE(voter.take_ready().persist_hard_state);
}

TEST(RaftElection, TermsOnlyEverIncrease) {
  Cluster cluster(3, 99);
  Term last = 0;
  for (int tick = 0; tick < 500; ++tick) {
    cluster.tick_all();
    ASSERT_TRUE(cluster.settle());
    const Term now = cluster.highest_term();
    ASSERT_GE(now, last);
    last = now;
  }
}

}  // namespace
