#include "sim/scheduler.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "sim/trace.h"

namespace {

using sim::EventKind;
using sim::EventTag;
using sim::Scheduler;
using sim::Trace;
using sim::TraceEvent;

EventTag tag(EventKind kind = EventKind::kTimer, base::u32 node = 0, base::u64 a = 0,
             base::u64 b = 0) {
  return EventTag{kind, node, a, b};
}

TEST(Scheduler, RunsEventsInTimeOrder) {
  Trace trace;
  Scheduler scheduler(trace);
  std::vector<int> order;

  scheduler.schedule_after(base::millis(30), tag(), [&] { order.push_back(30); });
  scheduler.schedule_after(base::millis(10), tag(), [&] { order.push_back(10); });
  scheduler.schedule_after(base::millis(20), tag(), [&] { order.push_back(20); });

  while (scheduler.run_next_before(base::seconds(1))) {
  }
  EXPECT_EQ(order, (std::vector<int>{10, 20, 30}));
  EXPECT_EQ(scheduler.now(), base::millis(30));
}

// Under a real clock, two events sharing a nanosecond essentially never happens. Under
// virtual time it is the common case, because the clock advances directly onto
// deadlines — so the tie-break has to be total, and it has to be insertion order.
TEST(Scheduler, TiesBreakByInsertionOrderNotHeapLayout) {
  Trace trace;
  Scheduler scheduler(trace);
  std::vector<int> order;

  for (int i = 0; i < 32; ++i) {
    scheduler.schedule_at(base::millis(5), tag(), [&order, i] { order.push_back(i); });
  }
  while (scheduler.run_next_before(base::seconds(1))) {
  }

  std::vector<int> expected(32);
  for (int i = 0; i < 32; ++i) expected[static_cast<std::size_t>(i)] = i;
  EXPECT_EQ(order, expected);
}

TEST(Scheduler, TimeNeverMovesBackwards) {
  Trace trace;
  Scheduler scheduler(trace);

  scheduler.advance_to(base::seconds(5));
  EXPECT_EQ(scheduler.now(), base::seconds(5));
  scheduler.advance_to(base::seconds(1));
  EXPECT_EQ(scheduler.now(), base::seconds(5));

  // An event scheduled in the past is due immediately rather than rejected: a handler
  // that consumed virtual time can legitimately overtake a deadline set before it ran.
  bool ran = false;
  scheduler.schedule_at(base::seconds(1), tag(), [&] { ran = true; });
  EXPECT_TRUE(scheduler.run_next_before(base::seconds(5)));
  EXPECT_TRUE(ran);
  EXPECT_EQ(scheduler.now(), base::seconds(5));
}

TEST(Scheduler, RunNextRespectsTheLimit) {
  Trace trace;
  Scheduler scheduler(trace);
  scheduler.schedule_after(base::millis(10), tag(), [] {});

  EXPECT_FALSE(scheduler.run_next_before(base::millis(9)));
  EXPECT_EQ(scheduler.now(), 0);
  EXPECT_TRUE(scheduler.run_next_before(base::millis(10)));
}

TEST(Scheduler, CancelledEventsNeverRun) {
  Trace trace;
  Scheduler scheduler(trace);
  int ran = 0;

  const sim::EventId first = scheduler.schedule_after(base::millis(10), tag(), [&] { ++ran; });
  scheduler.schedule_after(base::millis(20), tag(), [&] { ++ran; });
  EXPECT_TRUE(scheduler.cancel(first));
  EXPECT_FALSE(scheduler.cancel(first));  // cancelling twice is not two cancellations

  // The cancelled event is also gone from the deadline, or the simulation would advance
  // its clock to an event that never happens.
  EXPECT_EQ(scheduler.next_deadline(), base::millis(20));
  while (scheduler.run_next_before(base::seconds(1))) {
  }
  EXPECT_EQ(ran, 1);
}

TEST(Scheduler, EmptyQueueReportsNeverRatherThanNow) {
  Trace trace;
  Scheduler scheduler(trace);
  EXPECT_TRUE(scheduler.empty());
  EXPECT_EQ(scheduler.next_deadline(), base::kNoTimeout);

  // kNoTimeout is -1, so anything comparing deadlines numerically would treat "nothing
  // scheduled" as "due before everything" and stop the simulation at time zero.
  EXPECT_EQ(sim::earlier_deadline(base::kNoTimeout, base::millis(4)), base::millis(4));
  EXPECT_EQ(sim::earlier_deadline(base::millis(4), base::kNoTimeout), base::millis(4));
  EXPECT_EQ(sim::earlier_deadline(base::kNoTimeout, base::kNoTimeout), base::kNoTimeout);
  EXPECT_EQ(sim::earlier_deadline(base::millis(9), base::millis(4)), base::millis(4));
}

TEST(Scheduler, HandlersMayScheduleMoreWork) {
  Trace trace;
  Scheduler scheduler(trace);
  int count = 0;

  // A chain of five, each scheduling the next from inside its own callback.
  std::function<void()> step = [&] {
    if (++count < 5) scheduler.schedule_after(base::millis(1), tag(), step);
  };
  scheduler.schedule_after(base::millis(1), tag(), step);

  while (scheduler.run_next_before(base::seconds(1))) {
  }
  EXPECT_EQ(count, 5);
  EXPECT_EQ(scheduler.now(), base::millis(5));
}

TEST(Trace, HashDependsOnEveryField) {
  const TraceEvent base_event{base::millis(1), 2, EventKind::kAppend, 30, 40};

  Trace reference;
  reference.record(base_event);

  const std::vector<TraceEvent> mutations = {
      {base::millis(2), 2, EventKind::kAppend, 30, 40},
      {base::millis(1), 3, EventKind::kAppend, 30, 40},
      {base::millis(1), 2, EventKind::kFsync, 30, 40},
      {base::millis(1), 2, EventKind::kAppend, 31, 40},
      {base::millis(1), 2, EventKind::kAppend, 30, 41},
  };
  for (const TraceEvent& mutated : mutations) {
    Trace other;
    other.record(mutated);
    EXPECT_NE(other.hash(), reference.hash())
        << "a field changed without changing the hash: " << Trace::format(mutated);
  }
}

TEST(Trace, HashIsOrderSensitive) {
  const TraceEvent first{1, 0, EventKind::kAppend, 0, 0};
  const TraceEvent second{2, 1, EventKind::kFsync, 0, 0};

  Trace forward;
  forward.record(first);
  forward.record(second);

  Trace backward;
  backward.record(second);
  backward.record(first);

  // Two runs that do the same things in a different order are not the same run — that
  // is the whole class of bug the canary exists to catch.
  EXPECT_NE(forward.hash(), backward.hash());
}

TEST(Trace, RecentKeepsTheNewestWindowInOrder) {
  Trace trace(4);
  for (base::u64 i = 0; i < 10; ++i) {
    trace.record(TraceEvent{static_cast<base::Nanos>(i), 0, EventKind::kTimer, i, 0});
  }

  const std::vector<TraceEvent> recent = trace.recent();
  ASSERT_EQ(recent.size(), 4u);
  EXPECT_EQ(recent.front().a, 6u);
  EXPECT_EQ(recent.back().a, 9u);
  EXPECT_EQ(trace.count(), 10u);
}

TEST(Trace, RecentIsShortBeforeTheRingWraps) {
  Trace trace(8);
  trace.record(TraceEvent{1, 0, EventKind::kTimer, 1, 0});
  trace.record(TraceEvent{2, 0, EventKind::kTimer, 2, 0});

  const std::vector<TraceEvent> recent = trace.recent();
  ASSERT_EQ(recent.size(), 2u);
  EXPECT_EQ(recent.front().a, 1u);
  EXPECT_EQ(recent.back().a, 2u);
}

TEST(Trace, ScheduledEventsAreTracedWhenTheyFire) {
  Trace trace;
  Scheduler scheduler(trace);

  scheduler.schedule_after(base::millis(7), tag(EventKind::kAck, 1, 100, 4), [] {});
  EXPECT_EQ(trace.count(), 0u) << "scheduling is not an event; firing is";

  ASSERT_TRUE(scheduler.run_next_before(base::seconds(1)));
  ASSERT_EQ(trace.count(), 1u);

  const TraceEvent event = trace.recent().front();
  EXPECT_EQ(event.time, base::millis(7));
  EXPECT_EQ(event.node, 1u);
  EXPECT_EQ(event.kind, EventKind::kAck);
  EXPECT_EQ(event.a, 100u);
  EXPECT_EQ(event.b, 4u);
}

}  // namespace
