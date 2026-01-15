#include "sim/scheduler.h"

#include <algorithm>
#include <utility>

namespace sim {

EventId Scheduler::schedule_at(base::Nanos when, EventTag tag, Callback callback) {
  Event event;
  // An event scheduled in the past runs at the current instant rather than being
  // rejected. It happens legitimately: a handler that took virtual time to run can
  // overtake a deadline set before it started.
  event.when = when < now_ ? now_ : when;
  event.id = next_id_++;
  event.tag = tag;
  event.callback = std::move(callback);

  const EventId id = event.id;
  queue_.push_back(std::move(event));
  std::push_heap(queue_.begin(), queue_.end(), LaterFirst{});
  return id;
}

bool Scheduler::cancel(EventId id) {
  if (id == kInvalidEvent) return false;
  return cancelled_.insert(id).second;
}

void Scheduler::record(EventTag tag) {
  trace_.record(TraceEvent{now_, tag.node, tag.kind, tag.a, tag.b});
}

void Scheduler::drop_cancelled_from_top() {
  while (!queue_.empty()) {
    const auto it = cancelled_.find(queue_.front().id);
    if (it == cancelled_.end()) return;
    cancelled_.erase(it);
    std::pop_heap(queue_.begin(), queue_.end(), LaterFirst{});
    queue_.pop_back();
  }
}

base::Nanos Scheduler::next_deadline() {
  drop_cancelled_from_top();
  if (queue_.empty()) return base::kNoTimeout;
  return queue_.front().when;
}

bool Scheduler::empty() {
  drop_cancelled_from_top();
  return queue_.empty();
}

bool Scheduler::run_next_before(base::Nanos limit) {
  drop_cancelled_from_top();
  if (queue_.empty() || queue_.front().when > limit) return false;

  std::pop_heap(queue_.begin(), queue_.end(), LaterFirst{});
  Event event = std::move(queue_.back());
  queue_.pop_back();

  advance_to(event.when);
  record(event.tag);
  ++events_run_;

  // Fired after the trace line, not before: the line says the event happened, and a
  // handler that crashes a node or schedules more work should appear after its cause.
  if (event.callback) event.callback();
  return true;
}

}  // namespace sim
