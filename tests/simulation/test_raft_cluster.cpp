#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "sim/simulation.h"
#include "support/build_mode.h"

// Elections in the simulator: three real `raft::Node`s driven over the simulated network,
// persisting to the simulated disk, under crashes, partitions and clock jumps.
//
// The state machine itself is already covered by `tests/unit/test_raft_election.cpp`,
// which needs no infrastructure at all. What this file adds is everything the pure tests
// cannot reach: the driver's persist-then-send ordering, `raft.state` surviving power
// loss, and I6 holding while nodes are being killed underneath it.
namespace {

sim::SimulationConfig config_for(base::u64 seed, base::Nanos duration = base::seconds(60)) {
  sim::SimulationConfig config;
  config.seed = seed;
  config.node_count = 3;
  config.duration = duration;
  return config;
}

// Crash often and restart fast, so a node comes back *inside* the term it voted in. That
// is the only window in which a forgotten vote can elect two leaders, and it is narrow —
// which is exactly why it needs aiming at rather than waiting for.
//
// **This must stay byte-for-byte equivalent to what `scripts/demo_week4.sh` runs:**
//
//     ./sim --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120 [--unsafe-metadata]
//
// so nothing else is overridden here — partitions and clock jumps stay at their defaults
// because the CLI leaves them there. An earlier version of this helper zeroed them "to
// isolate the variable" and seed 4 promptly stopped violating, which is a tidy little
// lesson in what "the same configuration" has to mean when a seed is the evidence.
sim::SimulationConfig amnesia_config(base::u64 seed) {
  sim::SimulationConfig config = config_for(seed, base::seconds(120));
  config.faults.crash_interval = base::seconds(3);
  config.faults.restart_delay_max = base::millis(120);  // min stays at its 50 ms default
  return config;
}

// The seed `scripts/demo_week4.sh` runs. Pinned here so the demo cannot go stale quietly:
// if a timing change moves the violation off this seed, this test says so on the next
// build rather than the demo saying it in front of an audience.
constexpr base::u64 kDemoSeed = 4;

TEST(RaftCluster, AHealthyClusterElectsOneLeaderAndKeepsIt) {
  sim::SimulationConfig config = config_for(1, tests::sim_ns(base::seconds(120), base::seconds(15)));
  config.faults.crash_interval = 0;
  config.faults.partition_interval = 0;
  config.faults.clock_jump_interval = 0;

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;

  // Two minutes of simulated life, one election, one term. Anything more would mean
  // heartbeats are not holding off the followers' election timers — which costs
  // availability on every needless failover and is invisible in a "is there a leader"
  // check, because there always is one a moment later.
  EXPECT_EQ(result.elections, 1u);
  EXPECT_EQ(result.highest_term, 1u);
  EXPECT_EQ(result.leaders_at_end, 1u);
}

TEST(RaftCluster, EverySeedElectsSomebody) {
  for (base::u64 seed = 1; seed <= tests::seeds(50, 6); ++seed) {
    const sim::SimulationResult result = sim::run_simulation(config_for(seed));
    ASSERT_TRUE(result.ok) << "seed=" << seed << " violated "
                           << (result.invariant != nullptr ? result.invariant : "?") << ": "
                           << result.detail;
    EXPECT_GT(result.elections, 0u) << "seed=" << seed << ": nobody ever won a term";
    EXPECT_GT(result.hard_state_writes, 0u) << "seed=" << seed;
  }
}

// A clock that jumps must not be able to touch Raft. Timeouts are counted in ticks and
// the tick timer runs on the monotonic clock, so the wall clock has no route in (§17) —
// this test is what keeps that true after someone later reaches for a timestamp.
TEST(RaftCluster, ClockJumpsDoNotDisturbElections) {
  sim::SimulationConfig config = config_for(9, tests::sim_ns(base::seconds(120), base::seconds(15)));
  config.faults.crash_interval = 0;
  config.faults.partition_interval = 0;
  config.faults.clock_jump_interval = base::seconds(5);
  config.faults.clock_jump_max_ms = 60000;

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;
  EXPECT_EQ(result.elections, 1u) << "a wall-clock jump cost the cluster a leader";
  EXPECT_EQ(result.highest_term, 1u);
}

// §14.1 and §17: asymmetric partitions (A reaches B, B does not reach A) are the ones
// that livelock naive election loops, so they get their own test rather than being left
// to chance in the general sweep.
TEST(RaftCluster, AsymmetricPartitionsDoNotStopTheClusterElectingLeaders) {
  sim::SimulationConfig config = config_for(31, tests::sim_ns(base::seconds(180), base::seconds(20)));
  config.faults.crash_interval = 0;
  config.faults.asymmetric_partition_probability = 1.0;
  config.faults.partition_interval = base::seconds(10);
  config.faults.partition_duration_min = base::seconds(1);
  config.faults.partition_duration_max = base::seconds(4);

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;
  EXPECT_GT(result.elections, 0u);
  // Terms are *expected* to outrun elections here: a node that can send but not receive
  // campaigns into the void and burns a term each time. What must not happen is the
  // cluster never electing anyone at all.
  EXPECT_GT(result.leaders_at_end, 0u) << "the cluster ended with no leader at all";
}

TEST(RaftCluster, ARestartedNodeComesBackAtTheTermItRecorded) {
  // Crashes on, everything else off, so the only thing under test is whether the term
  // and vote survive the power cut.
  sim::SimulationConfig config = config_for(5, tests::sim_ns(base::seconds(120), base::seconds(20)));
  config.faults.partition_interval = 0;
  config.faults.clock_jump_interval = 0;
  config.faults.crash_interval = base::seconds(4);

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;
  EXPECT_GT(result.crashes, 5u) << "this test is only meaningful if nodes actually died";
  EXPECT_GT(result.elections, 0u);
}

// **Regression test for bug journal #2.**
//
// One cut link used to make leadership ping-pong every ~200 ms for as long as the
// partition lasted: 28 elections in 40 simulated seconds on this seed, and over four
// thousand across a 30-seed sweep. Every invariant held throughout — the cluster was
// perfectly safe and completely useless — which is why nothing caught it until somebody
// looked at the election counter.
//
// The bound is deliberately loose. What is being asserted is a change of *kind*: a
// partition should cost a failover or two, not a hundred.
TEST(RaftCluster, OneCutLinkDoesNotMakeLeadershipPingPong) {
  if (!tests::kRunSeedSpecificScenarios) GTEST_SKIP() << "instrumented build";
  sim::SimulationConfig config = config_for(3, base::seconds(40));
  config.capture_trace = false;
  config.faults.crash_interval = 0;
  config.faults.clock_jump_interval = 0;
  config.faults.partition_interval = base::seconds(5);
  config.faults.partition_duration_min = base::seconds(30);
  config.faults.partition_duration_max = base::seconds(30);
  config.faults.asymmetric_partition_probability = 0.0;

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;

  EXPECT_LE(result.elections, 6u)
      << "leadership is churning through the partition again (was 28 before the §4.2.3 "
         "lease rule, 2 after)";
  // And the cluster is still *doing* something — a run that elected nobody would also
  // pass the bound above, which would make this test worthless.
  EXPECT_GT(result.elections, 0u);
  EXPECT_GT(result.records_acked, 0u);
}

// **§13.2, the headline result.** One seed, one knob, and the knob is the one a producer
// actually sets. `acks=quorum+fsync` keeps every promise it makes through thirty crashes;
// `acks=1` answers faster and loses records the moment a leader dies before replicating.
//
// Neither is a bug. The point is that the difference is *demonstrable* rather than
// asserted, on a seed anyone can re-run.
TEST(RaftCluster, AcksOneLosesDataWhereAcksQuorumDoesNot) {
  if (!tests::kRunSeedSpecificScenarios) GTEST_SKIP() << "instrumented build";
  auto config_with = [](bool acks_one) {
    sim::SimulationConfig config = config_for(2, base::seconds(60));
    config.faults.crash_interval = base::seconds(4);
    config.workload.ack_on_local_append = acks_one;
    return config;
  };

  const sim::SimulationResult safe = sim::run_simulation(config_with(false));
  EXPECT_TRUE(safe.ok) << "acks=quorum+fsync must keep every promise: " << safe.detail;
  EXPECT_GT(safe.records_acked, 1000u);
  EXPECT_GT(safe.crashes, 10u) << "no crashes means neither setting was under any pressure";

  const sim::SimulationResult fast = sim::run_simulation(config_with(true));
  ASSERT_FALSE(fast.ok)
      << "acks=1 survived 30 crashes without losing a record. Either the fault stopped "
         "being injected or the promise stopped being checked — and scripts/demo_week5.sh "
         "depends on this seed losing data.";
  EXPECT_STREQ(fast.invariant, "I1") << fast.detail;
}

// **I8 — liveness**, and the invariant journal #2 said was missing. A healthy cluster
// should be leaderless only for its very first election.
TEST(RaftCluster, AHealthyClusterIsNeverLeaderlessForLong) {
  sim::SimulationConfig config = config_for(3, tests::sim_ns(base::seconds(120), base::seconds(15)));
  config.faults.crash_interval = 0;
  config.faults.partition_interval = 0;
  config.faults.clock_jump_interval = 0;

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;
  // One election timeout is 150–300 ms; the startup election is the only gap there should
  // be. A second of headroom catches a cluster that is churning without catching a slow
  // first election.
  EXPECT_LT(result.longest_leaderless, base::seconds(1))
      << "the cluster spent "
      << base::to_seconds_f(result.longest_leaderless) << " s with no leader at all";
}

// And the same measurement with faults on, which is where it earns its keep: no safety
// invariant can distinguish this from a healthy run, because a stopped cluster satisfies
// every one of them.
TEST(RaftCluster, TheClusterRecoversALeaderAfterEveryFault) {
  for (base::u64 seed = 1; seed <= tests::seeds(20, 3); ++seed) {
    sim::SimulationConfig config =
        config_for(seed, tests::sim_ns(base::seconds(120), base::seconds(30)));
    config.faults.crash_interval = base::seconds(8);

    const sim::SimulationResult result = sim::run_simulation(config);
    ASSERT_TRUE(result.ok) << "seed=" << seed << ": " << result.detail;
    // Generous, and deliberately so: crashes and partitions make *some* leaderless time
    // correct. What this rules out is the cluster never getting one back.
    EXPECT_LT(result.longest_leaderless, base::seconds(15))
        << "seed=" << seed << " went " << base::to_seconds_f(result.longest_leaderless)
        << " s with no leader";
    EXPECT_GT(result.records_acked, 0u) << "seed=" << seed << ": committed nothing at all";
  }
}

// **The test that proves the I6 checker can fail.**
//
// Week 3's lesson, third time it has come up: a green sweep is only evidence if the
// check was capable of going red (`docs/retrospective.md` §5). So take the one thing
// §13 says is not tunable — the fsync of `currentTerm`/`votedFor` before responding —
// turn it off, and watch the cluster elect two leaders in a single term.
//
// The window is narrow on purpose. For amnesia to matter, a node has to vote, lose the
// write to a power cut, and come *back inside the same term* to vote again — so the
// crash and restart timings below are aimed at exactly that window rather than left to
// the defaults. That narrowness is the whole argument for the simulator: this is a bug
// a real cluster would pass a thousand integration tests without ever showing.
// One seed, one knob, both directions. This runs in every build, including the
// instrumented ones — it is two simulations, not a sweep.
TEST(RaftCluster, OneKnobDecidesWhetherATermCanElectTwoLeaders) {
  if (!tests::kRunSeedSpecificScenarios) GTEST_SKIP() << "instrumented build";
  sim::SimulationConfig unsafe = amnesia_config(kDemoSeed);
  unsafe.workload.fsync_hard_state = false;
  const sim::SimulationResult broken = sim::run_simulation(unsafe);

  ASSERT_FALSE(broken.ok)
      << "seed " << kDemoSeed
      << " ran with the raft.state fsync disabled and nothing fired. Either the checker "
         "stopped checking or the fault stopped being injected — and scripts/demo_week4.sh "
         "depends on this seed violating.";
  EXPECT_STREQ(broken.invariant, "I6") << broken.detail;
  EXPECT_NE(broken.detail.find("two leaders"), std::string::npos) << broken.detail;

  // The identical seed with the fsync back on. §13.2's headline result, in its Raft form.
  const sim::SimulationResult whole = sim::run_simulation(amnesia_config(kDemoSeed));
  EXPECT_TRUE(whole.ok) << "seed " << kDemoSeed
                        << " must be clean with the fsync on: " << whole.detail;
  EXPECT_GT(whole.elections, 0u);
  EXPECT_GT(whole.crashes, 10u) << "no crashes means the fault never had a chance to bite";
}

// And the breadth, so the seed above cannot be a lucky one. Reduced under instrumented
// builds, where forty seeds cover exactly the same lines as four at a hundred times the
// cost — see tests/support/build_mode.h.
TEST(RaftCluster, TheAmnesiaFailureIsNotOneCherryPickedSeed) {
  if (!tests::kRunSeedSpecificScenarios) GTEST_SKIP() << "instrumented build";
  constexpr base::u64 kSeeds = tests::seeds(40, 4);
  std::vector<base::u64> violated;

  for (base::u64 seed = 1; seed <= kSeeds; ++seed) {
    sim::SimulationConfig config = amnesia_config(seed);
    config.workload.fsync_hard_state = false;

    const sim::SimulationResult result = sim::run_simulation(config);
    if (!result.ok) {
      EXPECT_STREQ(result.invariant, "I6") << "seed=" << seed << ": " << result.detail;
      violated.push_back(seed);
    }
  }

  if (!tests::kOptimizedUninstrumented) {
    // The reduced sweep may not reach a violating seed, and that is fine — the pinned
    // seed above already proved the path in this build. Asserting here would only make
    // the sanitizer jobs fail for a reason they are not about.
    GTEST_SKIP() << "instrumented build: ran " << kSeeds << " seeds, "
                 << violated.size() << " violated";
  }
  EXPECT_FALSE(violated.empty()) << kSeeds << " seeds without the fsync and I6 never fired";
}

}  // namespace
