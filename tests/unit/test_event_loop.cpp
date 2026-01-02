#include "runtime/event_loop.h"

#include <gtest/gtest.h>

#include <vector>

#include "io/network.h"

namespace {

// A clock the test drives by hand. This is a preview of the week-3 arrangement:
// the loop cannot tell virtual time from CLOCK_MONOTONIC, which is exactly why the
// simulator will be able to run hours of cluster life in seconds.
class FakeClock final : public io::Clock {
 public:
  base::Nanos monotonic_now() override { return now_; }
  base::i64 wall_now_ms() override { return now_ / base::kNanosPerMilli; }
  void advance(base::Nanos delta) { now_ += delta; }

 private:
  base::Nanos now_ = 0;
};

// Network that never has anything to say. poll() records the timeout it was asked
// for, which is how the tests check the loop's blocking decisions.
class NullNetwork final : public io::Network {
 public:
  base::Result<io::ConnId> listen(base::u16, int) override {
    return base::fail(base::ErrorCode::kIoError);
  }
  base::Result<base::u16> local_port(io::ConnId) override {
    return base::fail(base::ErrorCode::kIoError);
  }
  base::Result<io::ConnId> accept(io::ConnId) override {
    return base::fail(base::ErrorCode::kWouldBlock);
  }
  base::Result<io::ConnId> connect(std::string_view, base::u16) override {
    return base::fail(base::ErrorCode::kIoError);
  }
  base::Result<std::size_t> read(io::ConnId, base::MutSlice) override {
    return base::fail(base::ErrorCode::kWouldBlock);
  }
  base::Result<std::size_t> write(io::ConnId, base::Slice) override {
    return base::fail(base::ErrorCode::kWouldBlock);
  }
  void close(io::ConnId) override {}
  void watch(io::ConnId, io::Interest, io::ConnHandler*) override {}

  std::size_t poll(base::Nanos timeout) override {
    last_timeout = timeout;
    ++poll_count;
    return 0;
  }

  base::Nanos last_timeout = -12345;
  int poll_count = 0;
};

struct Harness {
  FakeClock clock;
  NullNetwork network;
  runtime::EventLoop loop{clock, network};
};

TEST(EventLoop, PostRunsOnNextIteration) {
  Harness h;
  int ran = 0;
  h.loop.post([&] { ++ran; });

  EXPECT_EQ(ran, 0);
  h.loop.run_once(0);
  EXPECT_EQ(ran, 1);

  h.loop.run_once(0);
  EXPECT_EQ(ran, 1) << "a posted task must run exactly once";
}

// A task that posts another task must not extend the batch it is running in —
// otherwise a self-reposting task starves I/O forever without the loop ever noticing.
TEST(EventLoop, PostFromWithinTaskDefersToNextIteration) {
  Harness h;
  int outer = 0;
  int inner = 0;
  h.loop.post([&] {
    ++outer;
    h.loop.post([&] { ++inner; });
  });

  h.loop.run_once(0);
  EXPECT_EQ(outer, 1);
  EXPECT_EQ(inner, 0);

  h.loop.run_once(0);
  EXPECT_EQ(inner, 1);
}

TEST(EventLoop, TimerFiresOnlyAfterDeadline) {
  Harness h;
  int fired = 0;
  h.loop.add_timer_after(base::millis(100), [&] { ++fired; });

  h.loop.run_once(0);
  EXPECT_EQ(fired, 0);

  h.clock.advance(base::millis(99));
  h.loop.run_once(0);
  EXPECT_EQ(fired, 0);

  h.clock.advance(base::millis(1));
  h.loop.run_once(0);
  EXPECT_EQ(fired, 1);

  h.clock.advance(base::seconds(10));
  h.loop.run_once(0);
  EXPECT_EQ(fired, 1) << "a one-shot timer must not re-fire";
}

TEST(EventLoop, TimersFireInDeadlineOrder) {
  Harness h;
  std::vector<int> order;
  h.loop.add_timer_after(base::millis(30), [&] { order.push_back(3); });
  h.loop.add_timer_after(base::millis(10), [&] { order.push_back(1); });
  h.loop.add_timer_after(base::millis(20), [&] { order.push_back(2); });

  h.clock.advance(base::millis(100));
  h.loop.run_once(0);

  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 2);
  EXPECT_EQ(order[2], 3);
}

// Ties broken by insertion order, not by heap layout. Without this the simulator's
// trace hash would drift between runs the first time two timers shared a deadline —
// and virtual time makes exact ties common, not rare.
TEST(EventLoop, EqualDeadlinesFireInInsertionOrder) {
  Harness h;
  std::vector<int> order;
  for (int i = 0; i < 16; ++i) {
    h.loop.add_timer_after(base::millis(5), [&order, i] { order.push_back(i); });
  }

  h.clock.advance(base::millis(5));
  h.loop.run_once(0);

  ASSERT_EQ(order.size(), 16u);
  for (int i = 0; i < 16; ++i) EXPECT_EQ(order[static_cast<std::size_t>(i)], i);
}

TEST(EventLoop, CancelTimer) {
  Harness h;
  int fired = 0;
  const runtime::TimerId id = h.loop.add_timer_after(base::millis(10), [&] { ++fired; });

  EXPECT_TRUE(h.loop.cancel_timer(id));
  EXPECT_FALSE(h.loop.cancel_timer(id)) << "cancelling twice must be a no-op";

  h.clock.advance(base::millis(100));
  h.loop.run_once(0);
  EXPECT_EQ(fired, 0);
}

TEST(EventLoop, CancelOneOfManyLeavesTheRestOrdered) {
  Harness h;
  std::vector<int> order;
  h.loop.add_timer_after(base::millis(10), [&] { order.push_back(1); });
  const auto mid = h.loop.add_timer_after(base::millis(20), [&] { order.push_back(2); });
  h.loop.add_timer_after(base::millis(30), [&] { order.push_back(3); });

  ASSERT_TRUE(h.loop.cancel_timer(mid));
  EXPECT_EQ(h.loop.pending_timer_count(), 2u);

  h.clock.advance(base::millis(100));
  h.loop.run_once(0);

  ASSERT_EQ(order.size(), 2u);
  EXPECT_EQ(order[0], 1);
  EXPECT_EQ(order[1], 3);
}

// Blocking past a timer deadline would make every election timeout late.
TEST(EventLoop, BlocksOnlyUntilTheNextDeadline) {
  Harness h;
  h.loop.add_timer_after(base::millis(50), [] {});

  h.loop.run_once(base::kNoTimeout);
  EXPECT_EQ(h.network.last_timeout, base::millis(50));

  // An explicit cap that is tighter than the deadline wins.
  h.loop.run_once(base::millis(5));
  EXPECT_EQ(h.network.last_timeout, base::millis(5));
}

TEST(EventLoop, DoesNotBlockWhenWorkIsPending) {
  Harness h;
  h.loop.post([&] { h.loop.post([] {}); });

  h.loop.run_once(base::kNoTimeout);
  EXPECT_EQ(h.network.last_timeout, 0) << "must not sleep with a task already queued";
}

TEST(EventLoop, BlocksIndefinitelyWithNoTimers) {
  Harness h;
  h.loop.run_once(base::kNoTimeout);
  EXPECT_EQ(h.network.last_timeout, base::kNoTimeout);
}

TEST(EventLoop, StopEndsRun) {
  Harness h;
  int iterations = 0;
  h.loop.add_timer_after(base::millis(1), [&] {
    ++iterations;
    h.loop.stop();
  });

  h.clock.advance(base::millis(1));
  h.loop.run();
  EXPECT_EQ(iterations, 1);
  EXPECT_FALSE(h.loop.is_running());
}

}  // namespace
