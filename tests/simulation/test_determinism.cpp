#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "sim/simulation.h"
#include "support/build_mode.h"

// **I7 — the same seed produces a byte-identical event trace.**
//
// This is the required check (§14.3, §18). Everything else in the correctness story is
// downstream of it: a bug found under a seed is only evidence if that seed reproduces
// it, and determinism is not a property anyone can maintain by being careful. It breaks
// silently, the first time someone iterates an unordered_map, reads an uninitialized
// byte, or lets a pointer value decide an ordering — and it goes on looking fine until
// a bug refuses to reproduce weeks later, in the middle of debugging Raft.
//
// So it is tested, on every push, and when it fails the two traces get diffed.
namespace {

sim::SimulationConfig config_for(
    base::u64 seed,
    base::Nanos duration = tests::sim_ns(base::seconds(20), base::seconds(6))) {
  sim::SimulationConfig config;
  config.seed = seed;
  config.node_count = 3;
  config.duration = duration;
  return config;
}

TEST(Determinism, SameSeedProducesTheSameTraceHash) {
  for (base::u64 seed : {1ull, 2ull, 7ull, 0x3f2a91c4ull, 999983ull}) {
    const sim::SimulationResult first = sim::run_simulation(config_for(seed));
    const sim::SimulationResult second = sim::run_simulation(config_for(seed));

    EXPECT_EQ(first.trace_hash, second.trace_hash) << "seed=" << seed;
    EXPECT_EQ(first.trace_events, second.trace_events) << "seed=" << seed;
    EXPECT_EQ(first.scheduler_events, second.scheduler_events) << "seed=" << seed;
    // Every observable total, not just the hash. A hash that matched while the totals
    // diverged would mean the trace was not recording the thing that changed.
    EXPECT_EQ(first.records_acked, second.records_acked) << "seed=" << seed;
    EXPECT_EQ(first.crashes, second.crashes) << "seed=" << seed;
    EXPECT_EQ(first.partitions, second.partitions) << "seed=" << seed;
    EXPECT_EQ(first.bytes_lost_to_crashes, second.bytes_lost_to_crashes) << "seed=" << seed;
    EXPECT_EQ(first.bytes_delivered, second.bytes_delivered) << "seed=" << seed;
  }
}

// The other half of the canary, and the one that is easy to forget: a hash that is
// stable because the seed is being ignored would pass every test above.
TEST(Determinism, DifferentSeedsProduceDifferentRuns) {
  std::vector<base::u64> hashes;
  for (base::u64 seed = 1; seed <= tests::seeds(8, 4); ++seed) {
    hashes.push_back(sim::run_simulation(config_for(seed)).trace_hash);
  }
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    for (std::size_t j = i + 1; j < hashes.size(); ++j) {
      EXPECT_NE(hashes[i], hashes[j]) << "seeds " << i + 1 << " and " << j + 1;
    }
  }
}

TEST(Determinism, HoldsWithoutFaultsToo) {
  sim::SimulationConfig config = config_for(4242);
  config.faults.crash_interval = 0;
  config.faults.partition_interval = 0;
  config.faults.clock_jump_interval = 0;

  const sim::SimulationResult first = sim::run_simulation(config);
  const sim::SimulationResult second = sim::run_simulation(config);
  EXPECT_EQ(first.trace_hash, second.trace_hash);
  EXPECT_EQ(first.crashes, 0u);
  EXPECT_GT(first.records_acked, 0u);
}

TEST(Determinism, HoldsForOtherClusterSizes) {
  for (base::u32 nodes : {1u, 2u, 5u}) {
    sim::SimulationConfig config = config_for(31337);
    config.node_count = nodes;
    EXPECT_EQ(sim::run_simulation(config).trace_hash, sim::run_simulation(config).trace_hash)
        << "nodes=" << nodes;
  }
}

TEST(Determinism, CapturedTraceIsIdenticalEventForEvent) {
  sim::SimulationConfig config = config_for(0xBEEF, base::seconds(5));
  config.capture_trace = true;

  sim::Simulation first(config);
  first.run();
  sim::Simulation second(config);
  second.run();

  const std::vector<sim::TraceEvent>& a = first.trace().captured();
  const std::vector<sim::TraceEvent>& b = second.trace().captured();
  ASSERT_EQ(a.size(), b.size());
  ASSERT_GT(a.size(), 100u) << "a trace this short is not evidence of anything";

  // Compared line by line rather than by hash, because this is the test that says what
  // a hash mismatch would have meant — and the first divergent line is the whole
  // debugging story when the canary fires.
  for (std::size_t i = 0; i < a.size(); ++i) {
    ASSERT_EQ(sim::Trace::format(a[i]), sim::Trace::format(b[i]))
        << "traces diverge at event " << i;
  }
}

// A fault injector that never injects is the week-1 lesson wearing a different hat: a
// check that cannot fail. If this test ever goes quiet, every green run above it stops
// meaning anything.
TEST(Simulator, FaultsActuallyFire) {
  base::u64 crashes = 0;
  base::u64 partitions = 0;
  base::u64 unflushed_lost = 0;
  base::u64 resets = 0;

  // Faults that fire *promptly*, rather than a long run that eventually meets the default
  // intervals. That distinction has teeth: shortening this file's runs for instrumented
  // builds dropped them below the 20 s default crash interval, and this test immediately
  // went red with four zeroes — which is precisely its job. A check that stops checking
  // because somebody tuned an unrelated knob is the failure mode the whole file is about.
  // Full length in every build, unlike the rest of this file. Unflushed-write loss needs a
  // crash to land in the window between a write and its fsync, and week 5 made that window
  // narrow — every append is fsynced, and so is every replicated entry. Shortening the run
  // here would not make the test faster in any way that matters; it would make it stop
  // being able to observe the one fault it exists to prove is real.
  auto config_for_faults = [](base::u64 seed) {
    sim::SimulationConfig config = config_for(seed, base::seconds(20));
    config.faults.crash_interval = base::seconds(2);
    config.faults.partition_interval = base::seconds(2);
    config.faults.partition_duration_max = base::seconds(1);
    return config;
  };

  for (base::u64 seed = 1; seed <= tests::seeds(10, 4); ++seed) {
    const sim::SimulationResult result = sim::run_simulation(config_for_faults(seed));
    crashes += result.crashes;
    partitions += result.partitions;
    unflushed_lost += result.bytes_lost_to_crashes;
    resets += result.connections_reset;
  }

  EXPECT_GT(crashes, 0u);
  EXPECT_GT(partitions, 0u);
  EXPECT_GT(resets, 0u);
  // The whole reason the simulator exists: this is the fault `kill -9` provably cannot
  // produce (`docs/retrospective.md` §5), and week 2's demo could only mimic it by
  // corrupting a file by hand.
  EXPECT_GT(unflushed_lost, 0u);
}

// Every node runs the same workload on the same schedule, so with no faults they should
// do the same amount of work. This is the test that was missing when nodes 1 and 2 spent
// a week running at two-thirds speed: their timer deadlines were computed in each node's
// own monotonic frame — which carries a per-node boot offset — and then compared against
// shared simulator time, so an offset node's deadline was never the earliest and it only
// got to run when another node's timer dragged the clock past its due point.
//
// Nothing caught it. The totals looked plausible, and the run was perfectly
// deterministic, so the I7 canary was green throughout: **determinism is not
// correctness, it only makes correctness checkable.**
TEST(Simulator, EveryNodeDoesItsShareOfTheWork) {
  sim::SimulationConfig config =
      config_for(2024, tests::sim_ns(base::seconds(30), base::seconds(10)));
  config.node_count = 4;
  config.faults.crash_interval = 0;
  config.faults.partition_interval = 0;

  const sim::SimulationResult result = sim::run_simulation(config);
  ASSERT_TRUE(result.ok) << result.detail;
  ASSERT_EQ(result.ticks_per_node.size(), 4u);

  const base::u64 most =
      *std::max_element(result.ticks_per_node.begin(), result.ticks_per_node.end());
  const base::u64 least =
      *std::min_element(result.ticks_per_node.begin(), result.ticks_per_node.end());
  ASSERT_GT(least, 0u);

  // Raft ticks, not appends. Week 5 made only the leader append, so append counts now
  // measure who won elections rather than who is being scheduled — and this test is about
  // scheduling. Every node runs its tick timer on the same fixed interval with no jitter
  // at all, so the spread should be tiny; 5% leaves room for the partial interval at the
  // end of the run without leaving room for a node running at two-thirds speed.
  EXPECT_LE(static_cast<double>(most - least) / static_cast<double>(most), 0.05)
      << "node scheduling is lopsided: " << least << " vs " << most << " ticks";
}

TEST(Simulator, AckedWritesSurviveEverySeed) {
  for (base::u64 seed = 1; seed <= tests::seeds(50, 5); ++seed) {
    const sim::SimulationResult result = sim::run_simulation(config_for(seed));
    ASSERT_TRUE(result.ok) << "seed=" << seed << " violated "
                           << (result.invariant != nullptr ? result.invariant : "?") << ": "
                           << result.detail;
    EXPECT_GT(result.records_acked, 0u) << "seed=" << seed;
  }
}

// A wall-clock assertion is only meaningful in an optimized, uninstrumented build.
// Under ASan/UBSan/TSan the same hour takes far longer, and asserting a time bound there
// measures the sanitizer rather than the simulator — a test that fails for a reason it is
// not about is worse than no test. The detection lives in one place now, because week 4
// needed the same distinction for a different reason (`tests/support/build_mode.h`).
constexpr bool kTimingIsMeaningful = tests::kOptimizedUninstrumented;

// NFR-4: at least one simulated cluster-hour per five wall-clock seconds. The bound is
// deliberately loose — this runs on whatever CI machine is free — but a 10× regression
// would still trip it, and the day the simulator gets slow is the day seed counts start
// quietly dropping. Simulation speed *is* the correctness budget (`retrospective.md` §5).
TEST(Simulator, SimulatesAnHourFastEnoughToBeWorthRunning) {
  // A full hour under a sanitizer takes minutes and proves nothing the shorter run does
  // not — the stopwatch is already meaningless there, so paying for the long version
  // only buys a CI timeout.
  const base::Nanos duration = kTimingIsMeaningful ? base::seconds(3600) : base::seconds(20);
  sim::SimulationConfig config = config_for(11, duration);

  const auto started = std::chrono::steady_clock::now();
  const sim::SimulationResult result = sim::run_simulation(config);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const double seconds = std::chrono::duration<double>(elapsed).count();

  // The correctness half runs everywhere; only the stopwatch is build-dependent.
  ASSERT_TRUE(result.ok) << "seed=" << config.seed << ": " << result.detail;
  EXPECT_GT(result.records_acked, 0u);
  EXPECT_GT(result.trace_events, 10000u);

  if (!kTimingIsMeaningful) {
    GTEST_SKIP() << "instrumented or unoptimized build: " << seconds
                 << " s is the sanitizer's number, not the simulator's";
  }
  EXPECT_GE(result.simulated_node_hours(), 3.0);
  // 60 s against a measured ~2 s. The margin is deliberately enormous because this is a
  // wall-clock assertion sharing a machine with whatever else is running — it once
  // tripped simply because three sanitizer builds were saturating the CPU. It is a
  // regression detector, not a benchmark: at 30× headroom it still catches the day
  // somebody makes the simulator an order of magnitude slower, and never fires for a
  // busy runner. The real number belongs in `retrospective.md` §4, measured idle.
  EXPECT_LT(seconds, 60.0) << "one simulated hour took " << seconds << " s of wall clock";
}

}  // namespace
