#pragma once

#include <functional>
#include <vector>

#include "base/types.h"
#include "io/clock.h"
#include "io/network.h"

namespace runtime {

using TimerId = base::u64;
inline constexpr TimerId kInvalidTimer = 0;

// Callback event loop over an io::Network and an io::Clock.
//
// Deliberately not coroutines (§15.1). Coroutines are ergonomics, not capability,
// and they are the single most likely way to lose two weeks in week 1. If the
// callback style becomes genuinely unreadable, week 7 can retrofit them behind this
// same interface — or not, which is also a fine outcome.
//
// Single-threaded by contract. It takes its network and clock by reference, so in
// simulation the identical loop runs on virtual time with a simulated network and
// nothing above it can tell the difference.
class EventLoop {
 public:
  using Task = std::function<void()>;

  EventLoop(io::Clock& clock, io::Network& network) : clock_(clock), network_(network) {}

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  [[nodiscard]] io::Clock& clock() noexcept { return clock_; }
  [[nodiscard]] io::Network& network() noexcept { return network_; }

  // Runs on the next iteration, before any I/O polling.
  void post(Task task) { pending_.push_back(std::move(task)); }

  TimerId add_timer_after(base::Nanos delay, Task task);
  bool cancel_timer(TimerId id);

  // One iteration: posted tasks, then I/O (blocking at most until the next timer
  // deadline or max_block, whichever is sooner), then expired timers.
  // Returns the number of things it did — posted tasks + I/O events + fired timers.
  std::size_t run_once(base::Nanos max_block);

  void run();
  void stop() noexcept { running_ = false; }
  [[nodiscard]] bool is_running() const noexcept { return running_; }

  [[nodiscard]] std::size_t pending_timer_count() const noexcept { return timers_.size(); }
  [[nodiscard]] bool has_pending_tasks() const noexcept { return !pending_.empty(); }

  // When this loop next has something to do on its own, or base::kNoTimeout if it is
  // waiting purely on I/O.
  //
  // Exists for the simulator. Under a real clock the loop decides for itself how long
  // to block; under virtual time it cannot, because the thing it would be waiting for
  // may belong to a different node and time must not advance past that node's deadline.
  // So the simulator asks every loop when it next wants to wake, takes the earliest
  // across all of them, and moves the clock there.
  [[nodiscard]] base::Nanos next_timer_deadline() const { return next_deadline(); }

 private:
  struct Timer {
    base::Nanos deadline;
    TimerId id;
    Task task;
  };

  // Min-heap ordered by deadline, ties broken by id so ordering is total and
  // reproducible. Comparing only deadlines would let two timers scheduled for the
  // same virtual nanosecond fire in heap-dependent order — a determinism bug that
  // would not show up until the simulator's trace hashes started disagreeing.
  struct LaterFirst {
    bool operator()(const Timer& a, const Timer& b) const noexcept {
      if (a.deadline != b.deadline) return a.deadline > b.deadline;
      return a.id > b.id;
    }
  };

  std::size_t drain_pending();
  std::size_t fire_expired_timers();
  [[nodiscard]] base::Nanos next_deadline() const;

  io::Clock& clock_;
  io::Network& network_;

  std::vector<Timer> timers_;
  std::vector<Task> pending_;
  std::vector<Task> draining_;
  TimerId next_timer_id_ = 1;
  bool running_ = false;
};

}  // namespace runtime
