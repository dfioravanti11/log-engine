#pragma once

#include "io/clock.h"
#include "sim/scheduler.h"

namespace io::sim {

// Virtual time, read straight off the scheduler.
//
// One clock per node, not one per cluster, and the two clocks it exposes are treated
// very differently:
//
//   monotonic_now() carries a fixed per-node offset — nodes boot at different moments
//     and their monotonic clocks have no common origin. It is otherwise untouchable.
//     **No fault ever moves it backwards or jumps it**, because every timeout, election
//     deadline, and retry in the system is measured with it, and a monotonic clock that
//     jumps is not a fault the code is allowed to survive — it is a broken kernel.
//
//   wall_now_ms() is fair game. §14.1's clock-jump fault lives here and only here,
//     which is the whole reason §17 insists correctness never depend on wall time. If a
//     jump in this value can break the cluster, something is reading the wrong clock,
//     and this class is how that gets caught rather than assumed.
class SimClock final : public Clock {
 public:
  SimClock(const ::sim::Scheduler& scheduler, base::Nanos boot_offset, base::i64 wall_epoch_ms)
      : scheduler_(scheduler), boot_offset_(boot_offset), wall_epoch_ms_(wall_epoch_ms) {}

  base::Nanos monotonic_now() override { return scheduler_.now() + boot_offset_; }

  // This node's monotonic origin, in scheduler time.
  //
  // The simulator needs it to translate a deadline the node computed for itself back
  // into shared time. Nothing above the seam may read it — a node that could discover
  // its own offset could correct for it, and then the offset would stop catching the
  // cross-node comparisons it exists to catch.
  [[nodiscard]] base::Nanos boot_offset() const noexcept { return boot_offset_; }

  base::i64 wall_now_ms() override {
    return wall_epoch_ms_ + wall_skew_ms_ + scheduler_.now() / base::kNanosPerMilli;
  }

  // §14.1 — the clock jumps. Deliberately unbounded and deliberately signed: NTP
  // stepping a clock backwards is the case that breaks naive timestamp logic.
  void jump_wall_clock(base::i64 delta_ms) { wall_skew_ms_ += delta_ms; }

 private:
  const ::sim::Scheduler& scheduler_;
  base::Nanos boot_offset_;
  base::i64 wall_epoch_ms_;
  base::i64 wall_skew_ms_ = 0;
};

}  // namespace io::sim
