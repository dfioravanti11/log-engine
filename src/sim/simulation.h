#pragma once

#include <memory>
#include <string>
#include <vector>

#include "io/seeded_random.h"
#include "io/sim/sim_clock.h"
#include "io/sim/sim_disk.h"
#include "io/sim/sim_network.h"
#include "runtime/event_loop.h"
#include "sim/scheduler.h"
#include "sim/trace.h"
#include "sim/workload.h"

namespace sim {

struct FaultConfig {
  io::sim::DiskFaultConfig disk;
  io::sim::NetworkFaultConfig network;

  // Mean time between crashes, per node. Zero disables crashing entirely.
  base::Nanos crash_interval = base::seconds(20);
  base::Nanos restart_delay_min = base::millis(50);
  base::Nanos restart_delay_max = base::seconds(3);

  base::Nanos partition_interval = base::seconds(30);
  base::Nanos partition_duration_min = base::millis(200);
  base::Nanos partition_duration_max = base::seconds(5);
  // Asymmetric partitions — A reaches B, B cannot reach A — are the ones that livelock
  // naive election loops (§14.1), so they are not a rare special case here.
  double asymmetric_partition_probability = 0.35;

  base::Nanos clock_jump_interval = base::seconds(90);
  base::i64 clock_jump_max_ms = 5000;

  // Silent bit-flip corruption (§14.1), **off until there is replication to repair it**.
  // With one copy of the data, a flipped bit in a durable batch is a genuinely lost
  // acked record — §17's answer is "re-replicate from a peer", and there is no peer to
  // replicate from until week 4. Turning it on now would only teach the invariant
  // checker to report a loss the design never claimed to survive.
  base::Nanos corruption_interval = 0;
};

struct SimulationConfig {
  base::u64 seed = 1;
  base::u32 node_count = 3;
  base::Nanos duration = base::seconds(60);
  FaultConfig faults;
  NodeWorkload::Config workload;
  bool capture_trace = false;
  base::u64 max_events = 0;  // 0 = no cap
};

struct SimulationResult {
  base::u64 seed = 0;
  base::u64 trace_hash = 0;
  base::u64 trace_events = 0;
  base::u64 scheduler_events = 0;
  base::Nanos simulated_time = 0;
  base::u32 node_count = 0;

  base::u64 records_acked = 0;
  base::u64 appends = 0;
  // Per node, so a test can ask whether the cluster shared the work. A node that is
  // silently running slower than its peers is invisible in any total.
  //
  // Raft ticks rather than appends: every node ticks on the same fixed interval, whereas
  // since week 5 only the leader appends, so append counts measure who won elections
  // rather than who is being scheduled.
  std::vector<base::u64> ticks_per_node;
  // Raft (week 4).
  base::u64 raft_messages = 0;
  base::u64 hard_state_writes = 0;
  // Terms that produced a leader. Compared against `highest_term`, this is the cheapest
  // health signal the simulator has: elections far below terms means the cluster is
  // burning terms without electing anybody.
  base::u64 elections = 0;
  base::u64 highest_term = 0;
  base::u32 leaders_at_end = 0;
  // The longest stretch with no leader anywhere (I8). Under no faults this should be a
  // single election timeout — the one at startup. Anything larger is the cluster failing
  // to recover, which no safety invariant can see.
  base::Nanos longest_leaderless = 0;
  // One sample per failover — the time from losing a leader to having one again. NFR-3
  // asks for p50/p99 across ≥50 induced failures, and this is where they come from.
  std::vector<base::Nanos> leaderless_gaps;

  base::u64 crashes = 0;
  base::u64 partitions = 0;
  base::u64 bytes_lost_to_crashes = 0;
  base::u64 bytes_delivered = 0;
  base::u64 connections_reset = 0;

  bool ok = true;
  const char* invariant = nullptr;  // the one that broke, or null
  std::string detail;
  std::vector<TraceEvent> tail;  // the last events before the violation (§14.2)

  [[nodiscard]] double simulated_node_hours() const;
};

// One run of one seed (§14).
//
// Everything nondeterministic in the system is behind the io/ seam, and every one of
// those implementations draws from a single `SeededRandom`. That is the whole contract:
// the seed determines the run, so a failure is a reproducible artifact rather than an
// anecdote, and re-running it produces a byte-identical trace (I7).
class Simulation {
 public:
  explicit Simulation(SimulationConfig config);
  ~Simulation();

  Simulation(const Simulation&) = delete;
  Simulation& operator=(const Simulation&) = delete;

  SimulationResult run();

  [[nodiscard]] const Trace& trace() const noexcept { return trace_; }

 private:
  struct Node {
    base::u32 id = 0;
    std::string dir;
    base::u16 port = 0;
    // The disk and the clock are the machine; they outlive the process. The event loop
    // and the workload are the process, and a crash destroys them outright.
    std::unique_ptr<io::sim::SimDisk> disk;
    std::unique_ptr<io::sim::SimClock> clock;
    io::sim::SimNetwork* network = nullptr;
    std::unique_ptr<runtime::EventLoop> loop;
    std::unique_ptr<NodeWorkload> workload;
    bool up = false;
  };

  void boot(Node& node);
  void crash_node(base::u32 id);
  void schedule_crash(base::u32 id);
  void schedule_restart(base::u32 id);
  void schedule_partition();
  void schedule_clock_jump();
  void drain_nodes();
  [[nodiscard]] base::Nanos next_node_deadline() const;
  [[nodiscard]] SimulationResult snapshot(bool completed) const;

  SimulationConfig config_;
  Trace trace_;
  Scheduler scheduler_;
  io::SeededRandom rng_;
  io::sim::Fabric fabric_;
  Oracle oracle_;
  std::vector<std::unique_ptr<Node>> nodes_;

  base::u64 crashes_ = 0;
  base::u64 partitions_ = 0;
  // Harvested from each workload before its crash destroys it.
  base::u64 raft_messages_ = 0;
  base::u64 hard_state_writes_ = 0;
};

SimulationResult run_simulation(const SimulationConfig& config);

}  // namespace sim
