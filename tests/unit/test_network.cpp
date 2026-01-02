#include "io/real/real_network.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "io/real/real_clock.h"
#include "runtime/event_loop.h"

namespace {

using base::Slice;

// Collects readiness callbacks so a test can assert on them without a real server.
class RecordingHandler final : public io::ConnHandler {
 public:
  void on_readable(io::ConnId conn) override { readable.push_back(conn); }
  void on_writable(io::ConnId conn) override { writable.push_back(conn); }
  void on_hangup(io::ConnId conn) override { hangup.push_back(conn); }

  std::vector<io::ConnId> readable;
  std::vector<io::ConnId> writable;
  std::vector<io::ConnId> hangup;
};

// Binds an ephemeral port, connects to it, and returns both ends. Port 0 rather than
// a hardcoded number so parallel CI jobs cannot collide.
struct LoopbackPair {
  io::ConnId listener = io::kInvalidConn;
  io::ConnId client = io::kInvalidConn;
  io::ConnId server = io::kInvalidConn;
};

LoopbackPair make_pair(io::real::RealNetwork& net) {
  LoopbackPair pair;

  auto listener = net.listen(0, 16);
  EXPECT_TRUE(listener.ok());
  pair.listener = listener.value();

  auto port = net.local_port(pair.listener);
  EXPECT_TRUE(port.ok());
  EXPECT_NE(port.value(), 0);

  auto client = net.connect("127.0.0.1", port.value());
  EXPECT_TRUE(client.ok());
  pair.client = client.value();

  // The accept may not be ready the instant connect() returns.
  for (int attempt = 0; attempt < 100 && pair.server == io::kInvalidConn; ++attempt) {
    auto server = net.accept(pair.listener);
    if (server.ok()) {
      pair.server = server.value();
    } else {
      EXPECT_EQ(server.error(), base::ErrorCode::kWouldBlock);
      net.poll(base::millis(50));
    }
  }
  EXPECT_NE(pair.server, io::kInvalidConn);
  return pair;
}

TEST(RealNetwork, ListenAssignsEphemeralPort) {
  io::real::RealNetwork net;
  auto listener = net.listen(0, 16);
  ASSERT_TRUE(listener.ok());

  auto port = net.local_port(listener.value());
  ASSERT_TRUE(port.ok());
  EXPECT_GT(port.value(), 0);

  net.close(listener.value());
}

TEST(RealNetwork, AcceptWithNoPendingConnectionWouldBlock) {
  io::real::RealNetwork net;
  auto listener = net.listen(0, 16);
  ASSERT_TRUE(listener.ok());

  auto conn = net.accept(listener.value());
  ASSERT_FALSE(conn.ok());
  EXPECT_EQ(conn.error(), base::ErrorCode::kWouldBlock);

  net.close(listener.value());
}

TEST(RealNetwork, RoundTripOverLoopback) {
  io::real::RealNetwork net;
  LoopbackPair pair = make_pair(net);

  const std::string message = "ping";
  auto written = net.write(pair.client, Slice::from_string(message));
  ASSERT_TRUE(written.ok());
  EXPECT_EQ(written.value(), message.size());

  RecordingHandler handler;
  net.watch(pair.server, io::Interest::kRead, &handler);

  for (int i = 0; i < 100 && handler.readable.empty(); ++i) {
    net.poll(base::millis(50));
  }
  ASSERT_FALSE(handler.readable.empty());
  EXPECT_EQ(handler.readable.front(), pair.server);

  std::vector<base::u8> out(64);
  auto got = net.read(pair.server, base::MutSlice(out.data(), out.size()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(Slice(out.data(), got.value()), Slice::from_string(message));

  net.close(pair.client);
  net.close(pair.server);
  net.close(pair.listener);
}

// A clean peer close arrives as a readable event whose read() returns 0 — not as a
// hangup. Handlers rely on that ordering to drain whatever is still buffered before
// tearing the connection down.
TEST(RealNetwork, PeerCloseSurfacesAsZeroLengthRead) {
  io::real::RealNetwork net;
  LoopbackPair pair = make_pair(net);

  RecordingHandler handler;
  net.watch(pair.server, io::Interest::kRead, &handler);
  net.close(pair.client);

  for (int i = 0; i < 100 && handler.readable.empty(); ++i) {
    net.poll(base::millis(50));
  }
  ASSERT_FALSE(handler.readable.empty());

  std::vector<base::u8> out(64);
  auto got = net.read(pair.server, base::MutSlice(out.data(), out.size()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), 0u) << "clean close must read as 0 bytes, not an error";

  net.close(pair.server);
  net.close(pair.listener);
}

TEST(RealNetwork, ReadWithNothingPendingWouldBlock) {
  io::real::RealNetwork net;
  LoopbackPair pair = make_pair(net);

  std::vector<base::u8> out(16);
  auto got = net.read(pair.server, base::MutSlice(out.data(), out.size()));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error(), base::ErrorCode::kWouldBlock);

  net.close(pair.client);
  net.close(pair.server);
  net.close(pair.listener);
}

TEST(RealNetwork, OperationsOnUnknownConnectionFail) {
  io::real::RealNetwork net;
  std::vector<base::u8> out(16);

  auto got = net.read(9999, base::MutSlice(out.data(), out.size()));
  ASSERT_FALSE(got.ok());
  EXPECT_EQ(got.error(), base::ErrorCode::kNotFound);
}

TEST(RealNetwork, CloseReleasesTheSlot) {
  io::real::RealNetwork net;
  LoopbackPair pair = make_pair(net);
  const std::size_t before = net.open_connection_count();

  net.close(pair.client);
  EXPECT_EQ(net.open_connection_count(), before - 1);

  net.close(pair.server);
  net.close(pair.listener);
  EXPECT_EQ(net.open_connection_count(), 0u);
}

// poll() with a timeout and nothing to report must actually wait, not spin. A loop
// that returns immediately would burn a core and inflate every latency measurement
// taken on the same machine.
TEST(RealNetwork, PollHonoursTimeout) {
  io::real::RealNetwork net;
  io::real::RealClock clock;

  const base::Nanos start = clock.monotonic_now();
  const std::size_t events = net.poll(base::millis(50));
  const base::Nanos elapsed = clock.monotonic_now() - start;

  EXPECT_EQ(events, 0u);
  EXPECT_GE(elapsed, base::millis(40)) << "poll returned far too early";
}

// The event loop over the real network: a write on one end must wake the other.
TEST(RealNetwork, DrivesTheEventLoop) {
  io::real::RealClock clock;
  io::real::RealNetwork net;
  runtime::EventLoop loop(clock, net);

  LoopbackPair pair = make_pair(net);

  struct EchoOnce final : io::ConnHandler {
    EchoOnce(io::Network& n, runtime::EventLoop& l) : net(n), loop(l) {}
    void on_readable(io::ConnId conn) override {
      std::vector<base::u8> buf(64);
      auto got = net.read(conn, base::MutSlice(buf.data(), buf.size()));
      if (got.ok() && got.value() > 0) {
        received.assign(buf.begin(), buf.begin() + static_cast<long>(got.value()));
        loop.stop();
      }
    }
    void on_writable(io::ConnId) override {}
    void on_hangup(io::ConnId) override { loop.stop(); }

    io::Network& net;
    runtime::EventLoop& loop;
    std::vector<base::u8> received;
  };

  EchoOnce handler(net, loop);
  net.watch(pair.server, io::Interest::kRead, &handler);

  ASSERT_TRUE(net.write(pair.client, Slice::from_string("via-loop")).ok());

  // Safety net so a broken poll fails the test instead of hanging CI forever.
  loop.add_timer_after(base::seconds(5), [&] { loop.stop(); });
  loop.run();

  EXPECT_EQ(Slice(handler.received.data(), handler.received.size()),
            Slice::from_string("via-loop"));

  net.close(pair.client);
  net.close(pair.server);
  net.close(pair.listener);
}

}  // namespace
