#pragma once

#include <functional>
#include <set>
#include <vector>

#include "base/types.h"
#include "sim/trace.h"

namespace sim {

using EventId = base::u64;
inline constexpr EventId kInvalidEvent = 0;

// What a scheduled event will look like in the trace once it fires.
struct EventTag {
  EventKind kind = EventKind::kTimer;
  base::u32 node = 0;
  base::u64 a = 0;
  base::u64 b = 0;
};

// Virtual time and the global event queue (§14).
//
// Two properties matter and everything here exists to serve them:
//
// **Time only moves forward, and only to the next event.** There is no tick and no
// sleep — the clock jumps straight to whatever happens next, which is why an hour of
// cluster life costs seconds of wall clock (NFR-4). Idle time is free because nothing
// iterates over it.
//
// **Ordering is total.** Events are ordered by `(when, id)`, never by `when` alone.
// Under a real clock two events sharing a nanosecond essentially never happens; under
// virtual time it is the *common* case, because the scheduler advances directly onto
// deadlines and everything scheduled for that instant becomes simultaneous. Ordering
// on deadline alone would leave ties broken by heap layout, and the trace hash would
// start disagreeing with itself for reasons no diff could explain. Week 1's timer heap
// learned this the same way (`docs/retrospective.md` §5).
//
// Note what this class deliberately does *not* do: it never draws from the RNG. It is
// a pure priority queue over time. All the nondeterminism a run explores enters
// through the *latencies* the simulated network and disk draw when they schedule work,
// and through the order nodes are drained in. Randomizing the queue itself as well
// would make failures harder to reason about without exploring anything new.
class Scheduler {
 public:
  using Callback = std::function<void()>;

  explicit Scheduler(Trace& trace) : trace_(trace) {}

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  [[nodiscard]] base::Nanos now() const noexcept { return now_; }
  [[nodiscard]] base::u64 events_run() const noexcept { return events_run_; }
  [[nodiscard]] Trace& trace() noexcept { return trace_; }

  EventId schedule_at(base::Nanos when, EventTag tag, Callback callback);
  EventId schedule_after(base::Nanos delay, EventTag tag, Callback callback) {
    return schedule_at(now_ + (delay < 0 ? 0 : delay), tag, std::move(callback));
  }

  // Cancelling is a tombstone, not a removal: erasing from the middle of a heap costs
  // a linear scan, and cancels are rare enough that skipping them on the way out is
  // cheaper.
  bool cancel(EventId id);

  // Records a state transition that is not itself a scheduled event — an ack, a crash,
  // a message being dropped. These are the lines that make a trace diff readable.
  void record(EventTag tag);

  // `base::kNoTimeout` when the queue is empty. Callers must treat that as "never",
  // not as "now" — it is -1, which compares less than every real deadline.
  [[nodiscard]] base::Nanos next_deadline();
  [[nodiscard]] bool empty();

  // Never moves backwards. A no-op if `when` is already in the past, which happens
  // whenever an event handler runs long enough to overtake a later deadline.
  void advance_to(base::Nanos when) noexcept {
    if (when > now_) now_ = when;
  }

  // Runs the single earliest event whose deadline is at or before `limit`, advancing
  // time to it and tracing it. Returns false if there is no such event.
  bool run_next_before(base::Nanos limit);

 private:
  struct Event {
    base::Nanos when = 0;
    EventId id = kInvalidEvent;
    EventTag tag;
    Callback callback;
  };

  // Min-heap over (when, id). std::push_heap builds a max-heap, so the comparator is
  // spelled "later first".
  struct LaterFirst {
    bool operator()(const Event& a, const Event& b) const noexcept {
      if (a.when != b.when) return a.when > b.when;
      return a.id > b.id;
    }
  };

  void drop_cancelled_from_top();

  Trace& trace_;
  std::vector<Event> queue_;
  std::set<EventId> cancelled_;  // ordered, not hashed: ER-2 forbids unordered iteration
  base::Nanos now_ = 0;
  EventId next_id_ = 1;
  base::u64 events_run_ = 0;
};

// Earlier of two deadlines, treating base::kNoTimeout as "never" rather than as the
// smallest value it numerically is. Written once here because getting it wrong makes
// the simulation stop at time zero and look like it ran.
[[nodiscard]] constexpr base::Nanos earlier_deadline(base::Nanos a, base::Nanos b) noexcept {
  if (a == base::kNoTimeout) return b;
  if (b == base::kNoTimeout) return a;
  return a < b ? a : b;
}

}  // namespace sim
