#include "io/sim/sim_network.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "io/seeded_random.h"
#include "sim/scheduler.h"
#include "sim/trace.h"

namespace {

using base::MutSlice;
using base::Slice;
using io::ConnId;
using io::Interest;
using io::sim::Fabric;
using io::sim::NetworkFaultConfig;
using io::sim::SimNetwork;

constexpr base::u64 kSeed = 0x5EED'0003;
constexpr base::u16 kPort = 7000;

// Records what it was told, so a test can assert on readiness rather than infer it.
class Recorder final : public io::ConnHandler {
 public:
  void on_readable(ConnId conn) override { readable.push_back(conn); }
  void on_writable(ConnId conn) override { writable.push_back(conn); }
  void on_hangup(ConnId conn) override { hangups.push_back(conn); }

  void clear() {
    readable.clear();
    writable.clear();
    hangups.clear();
  }

  std::vector<ConnId> readable;
  std::vector<ConnId> writable;
  std::vector<ConnId> hangups;
};

class SimNetworkTest : public ::testing::Test {
 protected:
  SimNetworkTest() : scheduler_(trace_), rng_(kSeed) {}

  void SetUp() override {
    NetworkFaultConfig config;
    config.partial_write_probability = 0.0;  // opt in per test; most want a clean wire
    fabric_ = std::make_unique<Fabric>(scheduler_, rng_, config);
    a_ = &fabric_->add_node(0, "n0");
    b_ = &fabric_->add_node(1, "n1");
  }

  // Runs every scheduled delivery, so a test can say "and then the bytes arrive".
  void settle() {
    while (scheduler_.run_next_before(base::seconds(10))) {
    }
  }

  std::string read_all(SimNetwork& net, ConnId conn) {
    std::string out;
    std::vector<base::u8> buffer(256);
    while (true) {
      auto got = net.read(conn, MutSlice(buffer.data(), buffer.size()));
      if (!got || got.value() == 0) break;
      out.append(reinterpret_cast<const char*>(buffer.data()), got.value());
    }
    return out;
  }

  sim::Trace trace_;
  sim::Scheduler scheduler_;
  io::SeededRandom rng_;
  std::unique_ptr<Fabric> fabric_;
  SimNetwork* a_ = nullptr;
  SimNetwork* b_ = nullptr;
};

TEST_F(SimNetworkTest, ConnectAcceptAndTransfer) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());

  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok()) << base::to_string(client.error());

  // The listener does not hear about it until a round trip has passed, which is why a
  // client may legally write before the server has accepted.
  EXPECT_FALSE(b_->accept(listener.value()).ok());

  auto written = a_->write(client.value(), Slice::from_string("hello wire"));
  ASSERT_TRUE(written.ok());
  EXPECT_EQ(written.value(), 10u);

  settle();

  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok()) << base::to_string(server.error());
  EXPECT_EQ(read_all(*b_, server.value()), "hello wire");
}

TEST_F(SimNetworkTest, ConnectToAnUnknownHostOrDeadPortFails) {
  auto unknown = a_->connect("n7", kPort);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error(), base::ErrorCode::kNotFound);

  auto refused = a_->connect("n1", kPort);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error(), base::ErrorCode::kNotFound);
}

TEST_F(SimNetworkTest, ReadOnAnIdleConnectionWouldBlockRatherThanReportEof) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  settle();

  std::vector<base::u8> buffer(16);
  auto got = a_->read(client.value(), MutSlice(buffer.data(), buffer.size()));
  ASSERT_FALSE(got.ok());
  // A caught-up reader and a closed connection must never look the same.
  EXPECT_EQ(got.error(), base::ErrorCode::kWouldBlock);
}

// TCP does not hand byte 40 over before byte 39. Independent latencies per write would,
// and every "corrupt frame" that followed would be an artifact of the simulator.
TEST_F(SimNetworkTest, BytesArriveInTheOrderTheyWereWritten) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  settle();
  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok());

  std::string expected;
  for (int i = 0; i < 200; ++i) {
    const std::string chunk = "[" + std::to_string(i) + "]";
    auto written = a_->write(client.value(), Slice::from_string(chunk));
    ASSERT_TRUE(written.ok());
    ASSERT_EQ(written.value(), chunk.size());
    expected += chunk;
    // Deliveries land at now+latency, so interleaving writes with time passing is what
    // gives an out-of-order model the chance to misbehave.
    scheduler_.run_next_before(scheduler_.now() + base::micros(200));
  }
  settle();

  EXPECT_EQ(read_all(*b_, server.value()), expected) << "seed=" << kSeed;
}

TEST_F(SimNetworkTest, PeerCloseSurfacesAsEofAfterTheBufferDrains) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  ASSERT_TRUE(a_->write(client.value(), Slice::from_string("last words")).ok());
  settle();
  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok());

  a_->close(client.value());

  // The reader must still get the bytes that were already sent. Reporting the close
  // first would lose the final response of every request that raced a shutdown.
  std::vector<base::u8> buffer(64);
  auto got = b_->read(server.value(), MutSlice(buffer.data(), buffer.size()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value(), 10u);

  auto eof = b_->read(server.value(), MutSlice(buffer.data(), buffer.size()));
  ASSERT_TRUE(eof.ok());
  EXPECT_EQ(eof.value(), 0u) << "a clean close is 0 bytes, not an error";
}

TEST_F(SimNetworkTest, PartitionDropsBytesAndThenResetsTheConnection) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  settle();
  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok());

  Recorder handler;
  a_->watch(client.value(), Interest::kRead, &handler);

  fabric_->cut_both(0, 1);

  // Writes still succeed — a socket accepts into its send buffer — but nothing arrives.
  auto written = a_->write(client.value(), Slice::from_string("into the void"));
  ASSERT_TRUE(written.ok());
  settle();
  EXPECT_EQ(read_all(*b_, server.value()), "");
  EXPECT_GT(fabric_->bytes_dropped(), 0u);

  // And after the retransmit window, the connection dies.
  EXPECT_GT(fabric_->connections_reset(), 0u);
  a_->poll(0);
  EXPECT_EQ(handler.hangups.size(), 1u);

  auto after = a_->read(client.value(), MutSlice(nullptr, 0));
  ASSERT_FALSE(after.ok());
  EXPECT_EQ(after.error(), base::ErrorCode::kClosed);
}

// A→B works while B→A does not. This is the shape that livelocks a naive election loop,
// and a fault model that only cuts links in pairs cannot express it at all.
TEST_F(SimNetworkTest, PartitionsAreDirectional) {
  fabric_->cut(0, 1);
  EXPECT_TRUE(fabric_->is_cut(0, 1));
  EXPECT_FALSE(fabric_->is_cut(1, 0));

  auto listener = a_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());

  // n1 → n0 is still cut for *connect*, because a TCP handshake needs both directions.
  auto blocked = b_->connect("n0", kPort);
  EXPECT_FALSE(blocked.ok());

  fabric_->heal(0, 1);
  auto healed = b_->connect("n0", kPort);
  EXPECT_TRUE(healed.ok());
}

TEST_F(SimNetworkTest, SendWindowPushesBackRatherThanBufferingForever) {
  NetworkFaultConfig config;
  config.partial_write_probability = 0.0;
  config.send_window_bytes = 512;
  fabric_ = std::make_unique<Fabric>(scheduler_, rng_, config);
  a_ = &fabric_->add_node(0, "n0");
  b_ = &fabric_->add_node(1, "n1");

  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());

  const std::string chunk(400, 'x');
  ASSERT_TRUE(a_->write(client.value(), Slice::from_string(chunk)).ok());

  // 400 of 512 bytes are in flight, so the next write is clipped to what fits.
  auto clipped = a_->write(client.value(), Slice::from_string(chunk));
  ASSERT_TRUE(clipped.ok());
  EXPECT_EQ(clipped.value(), 112u);

  auto blocked = a_->write(client.value(), Slice::from_string("more"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error(), base::ErrorCode::kWouldBlock);

  // Once the bytes land, the window reopens.
  settle();
  EXPECT_TRUE(a_->write(client.value(), Slice::from_string("more")).ok());
}

TEST_F(SimNetworkTest, ShortWritesHappenAndAreReportedHonestly) {
  NetworkFaultConfig config;
  config.partial_write_probability = 1.0;
  fabric_ = std::make_unique<Fabric>(scheduler_, rng_, config);
  a_ = &fabric_->add_node(0, "n0");
  b_ = &fabric_->add_node(1, "n1");

  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  settle();
  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok());

  // Every caller claims to handle a short write; this is what finds out. Whatever the
  // wire reports it accepted is exactly what must arrive — no more, no less.
  const std::string payload(64, 'z');
  auto written = a_->write(client.value(), Slice::from_string(payload));
  ASSERT_TRUE(written.ok());
  EXPECT_LT(written.value(), payload.size()) << "seed=" << kSeed;
  EXPECT_GT(written.value(), 0u);

  settle();
  EXPECT_EQ(read_all(*b_, server.value()).size(), written.value());
}

TEST_F(SimNetworkTest, PollDispatchesReadinessToTheWatchingHandler) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  Recorder server_handler;
  b_->watch(listener.value(), Interest::kRead, &server_handler);

  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());

  EXPECT_EQ(b_->poll(0), 0u) << "nothing has arrived yet";
  settle();
  EXPECT_EQ(b_->poll(0), 1u);
  ASSERT_EQ(server_handler.readable.size(), 1u);
  EXPECT_EQ(server_handler.readable.front(), listener.value());

  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok());
  server_handler.clear();
  b_->watch(server.value(), Interest::kRead, &server_handler);

  ASSERT_TRUE(a_->write(client.value(), Slice::from_string("ping")).ok());
  EXPECT_EQ(b_->poll(0), 0u) << "still in flight";
  settle();
  EXPECT_EQ(b_->poll(0), 1u);
  EXPECT_EQ(server_handler.readable.size(), 1u);
}

TEST_F(SimNetworkTest, HangupFiresOnceNotOnEveryPoll) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  settle();

  Recorder handler;
  a_->watch(client.value(), Interest::kReadWrite, &handler);

  fabric_->kill_node_connections(1);
  a_->poll(0);
  a_->poll(0);
  a_->poll(0);
  EXPECT_EQ(handler.hangups.size(), 1u);
}

TEST_F(SimNetworkTest, KillingANodeResetsEverythingItHeld) {
  auto listener = b_->listen(kPort, 8);
  ASSERT_TRUE(listener.ok());
  auto client = a_->connect("n1", kPort);
  ASSERT_TRUE(client.ok());
  settle();
  auto server = b_->accept(listener.value());
  ASSERT_TRUE(server.ok());

  fabric_->kill_node_connections(1);

  auto dead = a_->write(client.value(), Slice::from_string("anyone there"));
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error(), base::ErrorCode::kClosed);

  // The listener is gone too, so a reconnect is refused until the node comes back.
  auto refused = a_->connect("n1", kPort);
  EXPECT_FALSE(refused.ok());
}

TEST_F(SimNetworkTest, SameSeedProducesTheSameWire) {
  auto run = [](base::u64 seed) {
    sim::Trace trace;
    sim::Scheduler scheduler(trace);
    io::SeededRandom rng(seed);
    Fabric fabric(scheduler, rng, NetworkFaultConfig{});

    SimNetwork& left = fabric.add_node(0, "n0");
    SimNetwork& right = fabric.add_node(1, "n1");
    auto listener = right.listen(kPort, 8);
    auto client = left.connect("n1", kPort);
    for (int i = 0; i < 50; ++i) {
      (void)left.write(client.value(), Slice::from_string("payload"));
      scheduler.run_next_before(scheduler.now() + base::millis(1));
    }
    while (scheduler.run_next_before(base::seconds(10))) {
    }
    (void)listener;
    return trace.hash();
  };

  EXPECT_EQ(run(kSeed), run(kSeed));
  EXPECT_NE(run(kSeed), run(kSeed + 1));
}

}  // namespace
