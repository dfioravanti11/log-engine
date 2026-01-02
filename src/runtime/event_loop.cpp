#include "runtime/event_loop.h"

#include <algorithm>

namespace runtime {

TimerId EventLoop::add_timer_after(base::Nanos delay, Task task) {
  const TimerId id = next_timer_id_++;
  timers_.push_back(Timer{clock_.monotonic_now() + (delay > 0 ? delay : 0), id,
                          std::move(task)});
  std::push_heap(timers_.begin(), timers_.end(), LaterFirst{});
  return id;
}

bool EventLoop::cancel_timer(TimerId id) {
  const auto it = std::find_if(timers_.begin(), timers_.end(),
                               [id](const Timer& t) { return t.id == id; });
  if (it == timers_.end()) return false;
  timers_.erase(it);
  std::make_heap(timers_.begin(), timers_.end(), LaterFirst{});
  return true;
}

base::Nanos EventLoop::next_deadline() const {
  if (timers_.empty()) return base::kNoTimeout;
  return timers_.front().deadline;
}

std::size_t EventLoop::drain_pending() {
  if (pending_.empty()) return 0;
  // Swap out first: a task that posts another task must not extend the batch it is
  // running in, or a self-reposting task would spin the loop forever without ever
  // reaching the I/O poll.
  draining_.swap(pending_);
  pending_.clear();
  for (Task& task : draining_) {
    task();
  }
  const std::size_t count = draining_.size();
  draining_.clear();
  return count;
}

std::size_t EventLoop::fire_expired_timers() {
  std::size_t fired = 0;
  const base::Nanos now = clock_.monotonic_now();
  while (!timers_.empty() && timers_.front().deadline <= now) {
    std::pop_heap(timers_.begin(), timers_.end(), LaterFirst{});
    Task task = std::move(timers_.back().task);
    timers_.pop_back();
    task();
    ++fired;
  }
  return fired;
}

std::size_t EventLoop::run_once(base::Nanos max_block) {
  std::size_t work = drain_pending();

  base::Nanos timeout = max_block;
  if (!pending_.empty()) {
    // A task posted during this iteration is already due; do not sleep on I/O.
    timeout = 0;
  } else {
    const base::Nanos deadline = next_deadline();
    if (deadline != base::kNoTimeout) {
      const base::Nanos until = deadline - clock_.monotonic_now();
      const base::Nanos capped = until > 0 ? until : 0;
      timeout = (max_block == base::kNoTimeout) ? capped : std::min(max_block, capped);
    }
  }

  work += network_.poll(timeout);
  work += fire_expired_timers();
  return work;
}

void EventLoop::run() {
  running_ = true;
  while (running_) {
    run_once(base::kNoTimeout);
  }
}

}  // namespace runtime
