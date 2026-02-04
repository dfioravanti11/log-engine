// failover — how long the cluster is leaderless after a leader dies (NFR-3).
//
// **Measured in the simulator, and that is a deliberate choice, not a shortcut.**
//
// The obvious way to get this number is to `kill -9` a real leader fifty times and watch a
// stopwatch. That works, and `scripts/demo_week6.sh` does exactly it once. What it cannot
// give you is a *distribution* you can defend: fifty real kills take minutes, land wherever
// the scheduler happens to put them, and produce a p99 out of fifty samples that changes
// every time you run it. Worse, none of it replays — a surprising outlier is gone.
//
// Here every failure lands at a point a seed chose, the whole sweep runs in seconds, and an
// outlier is `--seed N` away from being reproduced under a debugger. The trade is honest
// and worth stating in the README: this measures the *algorithm's* failover time — election
// timeout, campaign, vote round trip, all on virtual time — and excludes everything real
// hardware adds, which is process restart, page cache, and scheduler latency. The real
// cluster's number is larger and `demo_week6.sh` shows it.
//
//   ./failover --failures 200
//
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "bench/histogram.h"
#include "sim/simulation.h"

int main(int argc, char** argv) {
  base::u64 wanted = 200;
  base::i64 crash_every_s = 5;
  base::i64 duration_s = 120;
  base::u32 nodes = 3;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--failures" && has_value) {
      wanted = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--crash-s" && has_value) {
      crash_every_s = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--duration-s" && has_value) {
      duration_s = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--nodes" && has_value) {
      nodes = static_cast<base::u32>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr,
                   "usage: failover [--failures N] [--crash-s N] [--duration-s N]"
                   " [--nodes N]\n");
      return 2;
    }
  }

  bench::Histogram histogram;
  base::u64 seeds_run = 0;
  base::u64 crashes = 0;
  double node_hours = 0;

  for (base::u64 seed = 1; histogram.count() < wanted; ++seed) {
    sim::SimulationConfig config;
    config.seed = seed;
    config.node_count = nodes;
    config.duration = base::seconds(duration_s);
    config.faults.crash_interval = base::seconds(crash_every_s);
    // Crashes only. A partition that costs the majority makes "leaderless" the *correct*
    // state, and folding those stretches into a failover distribution would be measuring
    // the fault injector rather than the algorithm.
    config.faults.partition_interval = 0;
    config.faults.clock_jump_interval = 0;

    const sim::SimulationResult result = sim::run_simulation(config);
    if (!result.ok) {
      std::fprintf(stderr,
                   "failover: seed %llu violated %s — benchmarking a broken cluster would "
                   "be measuring nothing\n  %s\n",
                   static_cast<unsigned long long>(seed),
                   result.invariant != nullptr ? result.invariant : "?", result.detail.c_str());
      return 1;
    }

    ++seeds_run;
    crashes += result.crashes;
    node_hours += result.simulated_node_hours();

    // Drop the first gap of every run: that is the startup election, which is a cold start
    // and not a failover. Including it would flatter nothing — it is the *fastest* sample,
    // since no state has to be caught up — but it would still be the wrong event.
    for (std::size_t i = 1; i < result.leaderless_gaps.size(); ++i) {
      histogram.add(result.leaderless_gaps[i]);
      if (histogram.count() >= wanted) break;
    }
  }

  // NFR-3: p99 ≤ 3× the election timeout. The timeout is 15–30 ticks at 10 ms, so the
  // worst legitimate one is 300 ms and the bound is 900 ms.
  constexpr double kElectionTimeoutMaxMs = 300.0;
  constexpr double kBoundMs = 3 * kElectionTimeoutMaxMs;

  std::printf("failover time — %llu induced leader failures over %llu seeds\n",
              static_cast<unsigned long long>(histogram.count()),
              static_cast<unsigned long long>(seeds_run));
  std::printf("  conditions          %u nodes, a crash every %llds, %.1f simulated"
              " node-hours, %llu crashes\n",
              nodes, static_cast<long long>(crash_every_s), node_hours,
              static_cast<unsigned long long>(crashes));
  std::printf("  p50                 %8.1f ms\n", base::to_millis_f(histogram.percentile(50)));
  std::printf("  p99                 %8.1f ms\n", base::to_millis_f(histogram.percentile(99)));
  std::printf("  p99.9               %8.1f ms\n", base::to_millis_f(histogram.percentile(99.9)));
  std::printf("  max                 %8.1f ms\n", base::to_millis_f(histogram.max()));
  std::printf("  NFR-3 bound         %8.1f ms (3x the 300 ms election timeout)  -> %s\n",
              kBoundMs,
              base::to_millis_f(histogram.percentile(99)) <= kBoundMs ? "PASS" : "FAIL");
  std::printf("  measured on virtual time: election + campaign + vote round trip.\n");
  std::printf("  excludes process restart, page cache, and scheduler latency — the real\n");
  std::printf("  cluster's number is larger, and scripts/demo_week6.sh shows one of them.\n");

  return base::to_millis_f(histogram.percentile(99)) <= kBoundMs ? 0 : 1;
}
