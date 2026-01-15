#include "sim/simulation.h"

#include <algorithm>
#include <utility>

namespace sim {
namespace {

// Every node listens on the same port. They are separate machines with separate address
// spaces, so there is nothing to collide.
constexpr base::u16 kServicePort = 7000;

// How many times one node may be run at a single instant before the simulation calls it
// a livelock. A node processes everything ready in one run_once(), so anything past a
// handful means a handler is generating work as fast as it consumes it — a real bug,
// and one that would otherwise present as the simulator hanging.
constexpr int kMaxDrainIterations = 64;

// Likewise for the scheduler: how many events may fire at one virtual instant before the
// simulation decides the clock has stopped. Generous, because a burst of deliveries all
// landing on the same nanosecond is legitimate.
constexpr int kMaxEventsPerInstant = 100000;

std::string node_name(base::u32 id) { return "n" + std::to_string(id); }

}  // namespace

double SimulationResult::simulated_node_hours() const {
  const double seconds = base::to_seconds_f(simulated_time);
  return seconds * static_cast<double>(node_count) / 3600.0;
}

Simulation::Simulation(SimulationConfig config)
    : config_(std::move(config)),
      trace_(64),
      scheduler_(trace_),
      rng_(config_.seed),
      fabric_(scheduler_, rng_, config_.faults.network),
      oracle_(config_.node_count) {
  trace_.set_capture_all(config_.capture_trace);

  nodes_.reserve(config_.node_count);
  for (base::u32 id = 0; id < config_.node_count; ++id) {
    auto node = std::make_unique<Node>();
    node->id = id;
    node->dir = "/data/" + node_name(id);
    node->port = kServicePort;
    node->disk = std::make_unique<io::sim::SimDisk>(rng_, config_.faults.disk);
    // Nodes boot at different moments, so their monotonic clocks share no origin. Any
    // code that compares one node's monotonic reading against another's is wrong, and
    // this offset is what makes that wrongness show up.
    node->clock = std::make_unique<io::sim::SimClock>(
        scheduler_, base::millis(static_cast<base::i64>(id) * 137), 1'700'000'000'000);
    node->network = &fabric_.add_node(id, node_name(id));
    nodes_.push_back(std::move(node));
  }
}

Simulation::~Simulation() = default;

void Simulation::boot(Node& node) {
  node.disk->power_on();
  node.loop = std::make_unique<runtime::EventLoop>(*node.clock, *node.network);

  std::vector<NodeWorkload::Peer> peers;
  for (const auto& other : nodes_) {
    if (other->id == node.id) continue;
    peers.push_back(NodeWorkload::Peer{node_name(other->id), other->port});
  }

  node.workload = std::make_unique<NodeWorkload>(node.id, scheduler_, *node.loop, *node.disk,
                                                 rng_, oracle_, node.dir, node.port,
                                                 std::move(peers), config_.workload);
  if (auto started = node.workload->start(); !started) {
    // A node that cannot open its own log is down, not broken: the simulated disk may
    // be returning I/O errors. It will try again at the next restart.
    node.workload.reset();
    node.loop.reset();
    node.up = false;
    return;
  }
  node.up = true;
  scheduler_.record(EventTag{EventKind::kRestart, node.id, 0, 0});
}

void Simulation::crash_node(base::u32 id) {
  Node& node = *nodes_[id];
  if (!node.up) return;

  const base::u64 unflushed = node.disk->unflushed_bytes();

  // Order matters, and it is the opposite of what looks natural. The power goes first;
  // only then do the process's objects get destroyed. Destroying them first would let
  // their destructors run against a live disk — and storage::Log::~Log() writes the
  // sparse index on the way out, which is a courtesy no crashing machine extends.
  fabric_.kill_node_connections(id);
  node.disk->crash();
  // Take the counters before the process that owns them is destroyed. Reading them off
  // the live workloads at the end would only ever report the last generation, and a run
  // with five hundred crashes would claim it did almost no work.
  pongs_ += node.workload->pongs();
  node.workload.reset();
  node.loop.reset();
  node.up = false;

  ++crashes_;
  scheduler_.record(EventTag{EventKind::kCrash, id, unflushed, 0});

  schedule_restart(id);
}

void Simulation::schedule_restart(base::u32 id) {
  const base::Nanos delay = rng_.next_in_range(config_.faults.restart_delay_min,
                                               config_.faults.restart_delay_max);
  scheduler_.schedule_after(delay, EventTag{EventKind::kRestart, id, 0, 0}, [this, id] {
    boot(*nodes_[id]);
    // A boot can fail — the simulated disk may be returning I/O errors, which is an
    // actively exercised configuration. Arming the next *crash* in that case loses the
    // node forever: crash_node() sees `!up`, returns at its guard, and schedules nothing,
    // so the retry chain ends silently and the cluster quietly shrinks with no signal in
    // the result. A failed boot has to schedule another boot, not a crash.
    if (nodes_[id]->up) {
      schedule_crash(id);
    } else {
      schedule_restart(id);
    }
  });
}

void Simulation::schedule_crash(base::u32 id) {
  const base::Nanos mean = config_.faults.crash_interval;
  if (mean <= 0) return;
  const base::Nanos delay = rng_.next_in_range(mean / 2, mean * 3 / 2);
  scheduler_.schedule_after(delay, EventTag{EventKind::kTimer, id, 0, 0},
                            [this, id] { crash_node(id); });
}

void Simulation::schedule_partition() {
  const base::Nanos mean = config_.faults.partition_interval;
  if (mean <= 0 || nodes_.size() < 2) return;

  const base::Nanos delay = rng_.next_in_range(mean / 2, mean * 3 / 2);
  scheduler_.schedule_after(delay, EventTag{EventKind::kTimer, 0, 0, 0}, [this] {
    const auto count = static_cast<base::u64>(nodes_.size());
    const auto from = static_cast<base::u32>(rng_.next_below(count));
    auto to = static_cast<base::u32>(rng_.next_below(count - 1));
    if (to >= from) ++to;  // any node but itself, without rejection sampling

    const bool asymmetric =
        rng_.next_bool_with_probability(config_.faults.asymmetric_partition_probability);
    if (asymmetric) {
      fabric_.cut(from, to);
    } else {
      fabric_.cut_both(from, to);
    }
    ++partitions_;

    const base::Nanos duration = rng_.next_in_range(config_.faults.partition_duration_min,
                                                    config_.faults.partition_duration_max);
    scheduler_.schedule_after(duration, EventTag{EventKind::kTimer, from, to, 0},
                              [this, from, to, asymmetric] {
                                if (asymmetric) {
                                  fabric_.heal(from, to);
                                } else {
                                  fabric_.heal_both(from, to);
                                }
                              });
    schedule_partition();
  });
}

void Simulation::schedule_clock_jump() {
  const base::Nanos mean = config_.faults.clock_jump_interval;
  if (mean <= 0) return;

  const base::Nanos delay = rng_.next_in_range(mean / 2, mean * 3 / 2);
  scheduler_.schedule_after(delay, EventTag{EventKind::kTimer, 0, 0, 0}, [this] {
    const auto count = static_cast<base::u64>(nodes_.size());
    const auto id = static_cast<base::u32>(rng_.next_below(count));
    // Signed on purpose: NTP stepping a clock *backwards* is the case that breaks
    // naive timestamp logic, and it only ever touches the wall clock — nothing is
    // allowed to move a monotonic clock (§17).
    const base::i64 jump = rng_.next_in_range(-config_.faults.clock_jump_max_ms,
                                              config_.faults.clock_jump_max_ms);
    nodes_[id]->clock->jump_wall_clock(jump);
    schedule_clock_jump();
  });
}

base::Nanos Simulation::next_node_deadline() const {
  base::Nanos best = base::kNoTimeout;
  for (const auto& node : nodes_) {
    if (!node->up || node->loop == nullptr) continue;
    if (node->loop->has_pending_tasks()) return scheduler_.now();

    const base::Nanos local = node->loop->next_timer_deadline();
    if (local == base::kNoTimeout) continue;

    // Translate out of the node's clock and into the shared one. A node's timer deadline
    // is computed from *its* monotonic_now(), which carries that node's boot offset —
    // machines do not share a monotonic origin, and this simulator injects that fact
    // deliberately. Comparing those raw values against shared time, or worse advancing
    // the shared clock to one, mixes two coordinate systems: every node with a nonzero
    // offset then has its deadline overstated, never becomes the earliest, and only gets
    // to run when some other node's timer happens to drag the clock past its due time.
    //
    // Measured before this fix, 3 nodes over 30 s with no faults: node 0 acked 595
    // batches, nodes 1 and 2 managed 406 and 421 — a third of the intended work silently
    // missing, at a rate that was still perfectly deterministic and so sailed through the
    // I7 trace-hash canary. Determinism is not correctness; it only makes correctness
    // checkable.
    best = earlier_deadline(best, local - node->clock->boot_offset());
  }
  return best;
}

void Simulation::drain_nodes() {
  const std::size_t count = nodes_.size();
  if (count == 0) return;

  // The starting node rotates every round. Draining in a fixed order would mean node 0
  // always acts first at any instant two nodes are ready — a systematic bias reality
  // does not have, and one that would quietly hide any bug whose trigger is the other
  // order. One RNG draw buys the whole space of rotations across seeds.
  const auto start = static_cast<std::size_t>(rng_.next_below(count));

  for (std::size_t k = 0; k < count; ++k) {
    Node& node = *nodes_[(start + k) % count];
    if (!node.up || node.loop == nullptr) continue;

    int guard = 0;
    while (node.loop->run_once(0) > 0) {
      if (++guard >= kMaxDrainIterations) {
        oracle_.violation("LIVELOCK", "node " + std::to_string(node.id) +
                                          " never went quiet at a single instant");
        return;
      }
      if (!node.up || node.loop == nullptr) break;  // a handler took the node down
    }
  }
}

SimulationResult Simulation::snapshot(bool completed) const {
  SimulationResult result;
  result.seed = config_.seed;
  result.trace_hash = trace_.hash();
  result.trace_events = trace_.count();
  result.scheduler_events = scheduler_.events_run();
  result.simulated_time = completed ? config_.duration : scheduler_.now();
  result.node_count = config_.node_count;

  // Batches come from the oracle rather than the workloads: the oracle outlives every
  // crash, and the workloads do not.
  result.records_acked = oracle_.total_records();
  result.appends = oracle_.total_batches();
  result.pongs = pongs_;
  for (base::u32 id = 0; id < config_.node_count; ++id) {
    result.batches_per_node.push_back(static_cast<base::u64>(oracle_.acks(id).size()));
  }
  for (const auto& node : nodes_) {
    if (node->workload != nullptr) result.pongs += node->workload->pongs();
    result.bytes_lost_to_crashes += node->disk->bytes_lost_on_crash();
  }
  result.crashes = crashes_;
  result.partitions = partitions_;
  result.bytes_delivered = fabric_.bytes_delivered();
  result.connections_reset = fabric_.connections_reset();

  result.ok = oracle_.ok();
  result.invariant = oracle_.invariant();
  result.detail = oracle_.detail();
  if (!result.ok) result.tail = trace_.recent();
  return result;
}

SimulationResult Simulation::run() {
  for (const auto& node : nodes_) boot(*node);
  for (const auto& node : nodes_) schedule_crash(node->id);
  schedule_partition();
  schedule_clock_jump();

  const base::Nanos end = config_.duration;
  bool completed = false;

  while (oracle_.ok()) {
    // The clock jumps to whatever happens next, across both queues: the simulator's own
    // events and the nodes' timers. Idle time costs nothing because nothing iterates
    // over it, which is the entire reason an hour of cluster life fits in seconds.
    const base::Nanos next = earlier_deadline(scheduler_.next_deadline(), next_node_deadline());
    if (next == base::kNoTimeout || next > end) {
      completed = true;
      break;
    }

    scheduler_.advance_to(next);

    // Everything due at this instant, including anything those handlers schedule for the
    // same instant. Bounded because nothing today schedules with zero delay — every
    // latency and restart delay is strictly positive — but a future zero-delay event
    // that reschedules itself would otherwise hang the simulation with the clock frozen,
    // which reads as "the simulator is slow" rather than "there is a bug".
    int same_instant = 0;
    while (oracle_.ok() && scheduler_.run_next_before(scheduler_.now())) {
      if (++same_instant >= kMaxEventsPerInstant) {
        oracle_.violation("LIVELOCK", "virtual time stopped advancing: " +
                                          std::to_string(same_instant) +
                                          " events scheduled at a single instant");
        break;
      }
    }
    if (!oracle_.ok()) break;

    drain_nodes();

    if (config_.max_events != 0 && scheduler_.events_run() >= config_.max_events) break;
  }

  if (completed) scheduler_.advance_to(end);
  return snapshot(completed);
}

SimulationResult run_simulation(const SimulationConfig& config) {
  Simulation simulation(config);
  return simulation.run();
}

}  // namespace sim
