// bench/echo — week 1's demo.
//
// Drives request/response frames over a real loopback TCP socket, through the real
// event loop and the real framing codec, and reports throughput and latency
// percentiles. Its job is to prove the week-1 transport stack exists end to end and
// to establish the floor that every later number is measured against: nothing built
// on top of this can be faster than an empty echo.
//
// The server runs on its own thread with its own EventLoop and RealNetwork —
// shared-nothing, exactly as brokers will be. std::thread is legal here because
// bench/ sits outside the ER-1 guarded directories; nothing in src/server/ may do
// this.
//
// HONESTY NOTE, and it matters more than the numbers: this harness is CLOSED-LOOP.
// It keeps a fixed number of requests in flight and only issues a new one when a
// response lands, so if the server stalls, the load generator politely stalls with
// it and never records the queueing delay that a real producer would have suffered.
// That is coordinated omission, and it makes p99 look better than reality. The
// week-8 benchmarks (§19) use an open-loop generator with a fixed arrival rate for
// exactly this reason. Do not put a number from this program in the README.

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "base/buffer.h"
#include "io/real/real_clock.h"
#include "io/real/real_network.h"
#include "runtime/event_loop.h"
#include "wire/frame.h"

namespace {

struct Options {
  int duration_s = 5;
  std::size_t payload_bytes = 128;
  int pipeline_depth = 32;
  base::u32 max_frame_bytes = wire::kDefaultMaxFrameBytes;
};

// ---------------------------------------------------------------------------
// Server: decode a frame, write the identical frame back.
// ---------------------------------------------------------------------------
class EchoServer final : public io::ConnHandler {
 public:
  EchoServer(io::Network& net, io::ConnId listener, base::u32 max_frame_bytes)
      : net_(net), listener_(listener), decoder_(max_frame_bytes) {
    net_.watch(listener_, io::Interest::kRead, this);
  }

  void on_readable(io::ConnId conn) override {
    if (conn == listener_) {
      accept_pending();
      return;
    }
    if (!fill_from_socket(conn)) return;
    drain_frames(conn);
    flush(conn);
  }

  void on_writable(io::ConnId conn) override { flush(conn); }
  void on_hangup(io::ConnId conn) override { drop(conn); }

 private:
  void accept_pending() {
    while (true) {
      auto conn = net_.accept(listener_);
      if (!conn.ok()) return;  // kWouldBlock: nothing more pending
      net_.watch(conn.value(), io::Interest::kRead, this);
    }
  }

  bool fill_from_socket(io::ConnId conn) {
    constexpr std::size_t kReadChunk = 64 * 1024;
    while (true) {
      base::MutSlice dst = in_.append_uninitialized(kReadChunk);
      auto got = net_.read(conn, dst);
      if (!got.ok()) {
        in_.shrink_by(kReadChunk);
        if (got.error() == base::ErrorCode::kWouldBlock) return true;
        drop(conn);
        return false;
      }
      if (got.value() == 0) {  // clean peer close
        in_.shrink_by(kReadChunk);
        drop(conn);
        return false;
      }
      in_.shrink_by(kReadChunk - got.value());
      if (got.value() < kReadChunk) return true;
    }
  }

  void drain_frames(io::ConnId conn) {
    while (true) {
      wire::FrameHeader header;
      base::Slice payload;
      auto got = decoder_.next(in_, &header, &payload);
      if (!got.ok()) {  // protocol violation: close, never retry
        drop(conn);
        return;
      }
      if (!got.value()) return;
      wire::encode_frame(out_, header, payload);
      decoder_.consume_frame(in_);
    }
  }

  void flush(io::ConnId conn) {
    while (!out_.empty()) {
      auto written = net_.write(conn, out_.slice());
      if (!written.ok()) {
        if (written.error() == base::ErrorCode::kWouldBlock) {
          // Socket buffer is full: wait for writability rather than spinning.
          net_.watch(conn, io::Interest::kReadWrite, this);
          return;
        }
        drop(conn);
        return;
      }
      out_.consume(written.value());
    }
    net_.watch(conn, io::Interest::kRead, this);
  }

  void drop(io::ConnId conn) {
    net_.close(conn);
    in_.clear();
    out_.clear();
  }

  io::Network& net_;
  io::ConnId listener_;
  wire::FrameDecoder decoder_;
  base::Buffer in_;
  base::Buffer out_;
};

// ---------------------------------------------------------------------------
// Client: keep `pipeline_depth` requests in flight, time each round trip.
// ---------------------------------------------------------------------------
class EchoClient final : public io::ConnHandler {
 public:
  EchoClient(io::Network& net, runtime::EventLoop& loop, io::Clock& clock,
             io::ConnId conn, const Options& opts)
      : net_(net),
        loop_(loop),
        clock_(clock),
        conn_(conn),
        opts_(opts),
        decoder_(opts.max_frame_bytes),
        payload_(opts.payload_bytes, 0x41) {
    sent_at_.resize(static_cast<std::size_t>(opts.pipeline_depth), 0);
    latencies_.reserve(1u << 20);
    net_.watch(conn_, io::Interest::kRead, this);
  }

  void start(base::Nanos deadline) {
    deadline_ = deadline;
    for (int slot = 0; slot < opts_.pipeline_depth; ++slot) {
      send(static_cast<base::u32>(slot));
    }
    flush();
  }

  void on_readable(io::ConnId conn) override {
    if (!fill_from_socket(conn)) return;

    while (true) {
      wire::FrameHeader header;
      base::Slice payload;
      auto got = decoder_.next(in_, &header, &payload);
      if (!got.ok()) {
        std::fprintf(stderr, "client: protocol error %s\n", base::to_string(got.error()));
        loop_.stop();
        return;
      }
      if (!got.value()) break;

      const base::Nanos now = clock_.monotonic_now();
      const auto slot = static_cast<std::size_t>(header.correlation_id) %
                        sent_at_.size();
      latencies_.push_back(now - sent_at_[slot]);
      ++completed_;
      bytes_ += payload.size();
      decoder_.consume_frame(in_);

      if (now >= deadline_) {
        loop_.stop();
        return;
      }
      send(header.correlation_id);
    }
    flush();
  }

  void on_writable(io::ConnId) override { flush(); }
  void on_hangup(io::ConnId) override { loop_.stop(); }

  [[nodiscard]] base::u64 completed() const noexcept { return completed_; }
  [[nodiscard]] base::u64 bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::vector<base::Nanos>& latencies() noexcept { return latencies_; }

 private:
  void send(base::u32 correlation_id) {
    const auto slot = static_cast<std::size_t>(correlation_id) % sent_at_.size();
    sent_at_[slot] = clock_.monotonic_now();
    wire::encode_frame(out_,
                       wire::FrameHeader{wire::ApiKey::kEcho, wire::kApiVersion0,
                                         correlation_id},
                       base::Slice(payload_.data(), payload_.size()));
  }

  bool fill_from_socket(io::ConnId conn) {
    constexpr std::size_t kReadChunk = 64 * 1024;
    while (true) {
      base::MutSlice dst = in_.append_uninitialized(kReadChunk);
      auto got = net_.read(conn, dst);
      if (!got.ok()) {
        in_.shrink_by(kReadChunk);
        if (got.error() == base::ErrorCode::kWouldBlock) return true;
        loop_.stop();
        return false;
      }
      if (got.value() == 0) {
        in_.shrink_by(kReadChunk);
        loop_.stop();
        return false;
      }
      in_.shrink_by(kReadChunk - got.value());
      if (got.value() < kReadChunk) return true;
    }
  }

  void flush() {
    while (!out_.empty()) {
      auto written = net_.write(conn_, out_.slice());
      if (!written.ok()) {
        if (written.error() == base::ErrorCode::kWouldBlock) {
          net_.watch(conn_, io::Interest::kReadWrite, this);
          return;
        }
        loop_.stop();
        return;
      }
      out_.consume(written.value());
    }
    net_.watch(conn_, io::Interest::kRead, this);
  }

  io::Network& net_;
  runtime::EventLoop& loop_;
  io::Clock& clock_;
  io::ConnId conn_;
  Options opts_;
  wire::FrameDecoder decoder_;
  std::vector<base::u8> payload_;
  std::vector<base::Nanos> sent_at_;
  std::vector<base::Nanos> latencies_;
  base::Buffer in_;
  base::Buffer out_;
  base::Nanos deadline_ = 0;
  base::u64 completed_ = 0;
  base::u64 bytes_ = 0;
};

base::Nanos percentile(std::vector<base::Nanos>& sorted, double p) {
  if (sorted.empty()) return 0;
  const auto n = static_cast<double>(sorted.size() - 1);
  auto idx = static_cast<std::size_t>(p * n);
  if (idx >= sorted.size()) idx = sorted.size() - 1;
  return sorted[idx];
}

Options parse_args(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = (i + 1) < argc;
    if (arg == "--duration-s" && has_next) {
      opts.duration_s = std::atoi(argv[++i]);
    } else if (arg == "--payload-bytes" && has_next) {
      opts.payload_bytes = static_cast<std::size_t>(std::atoll(argv[++i]));
    } else if (arg == "--pipeline" && has_next) {
      opts.pipeline_depth = std::atoi(argv[++i]);
    } else if (arg == "--help") {
      std::printf(
          "usage: echo [--duration-s N] [--payload-bytes N] [--pipeline N]\n");
      std::exit(0);
    }
  }
  if (opts.duration_s < 1) opts.duration_s = 1;
  if (opts.pipeline_depth < 1) opts.pipeline_depth = 1;
  return opts;
}

}  // namespace

int main(int argc, char** argv) {
  const Options opts = parse_args(argc, argv);

  // --- server on its own thread, shared-nothing ---
  io::real::RealNetwork server_net;
  auto listener = server_net.listen(0, 128);
  if (!listener.ok()) {
    std::fprintf(stderr, "listen failed: %s\n", base::to_string(listener.error()));
    return 1;
  }
  auto port = server_net.local_port(listener.value());
  if (!port.ok()) {
    std::fprintf(stderr, "local_port failed: %s\n", base::to_string(port.error()));
    return 1;
  }

  std::atomic<bool> server_ready{false};
  std::thread server_thread([&] {
    io::real::RealClock clock;
    runtime::EventLoop loop(clock, server_net);
    EchoServer server(server_net, listener.value(), opts.max_frame_bytes);
    server_ready.store(true, std::memory_order_release);
    loop.run();
  });

  while (!server_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // --- client on the main thread ---
  io::real::RealClock clock;
  io::real::RealNetwork client_net;
  runtime::EventLoop loop(clock, client_net);

  auto conn = client_net.connect("127.0.0.1", port.value());
  if (!conn.ok()) {
    std::fprintf(stderr, "connect failed: %s\n", base::to_string(conn.error()));
    return 1;
  }

  EchoClient client(client_net, loop, clock, conn.value(), opts);

  const base::Nanos started = clock.monotonic_now();
  const base::Nanos deadline = started + base::seconds(opts.duration_s);
  client.start(deadline);

  // Hard stop so a hung socket fails the demo instead of hanging it.
  loop.add_timer_after(base::seconds(opts.duration_s) + base::seconds(5),
                       [&] { loop.stop(); });
  loop.run();
  const base::Nanos elapsed = clock.monotonic_now() - started;

  client_net.close(conn.value());

  // The server loop blocks in poll(); closing its listener is not enough to wake it,
  // so the process exits with the thread detached. A broker gets a proper wakeup
  // (an eventfd/self-pipe) when the MPSC queue lands in week 7 — a benchmark harness
  // does not need one.
  server_thread.detach();

  auto& latencies = client.latencies();
  std::sort(latencies.begin(), latencies.end());

  const double seconds = base::to_seconds_f(elapsed);
  const double rps = seconds > 0 ? static_cast<double>(client.completed()) / seconds : 0;
  const double mibps =
      seconds > 0 ? static_cast<double>(client.bytes()) / seconds / (1024.0 * 1024.0) : 0;

  std::printf("\nbench/echo — week 1 transport floor\n");
  std::printf("  payload            %zu B\n", opts.payload_bytes);
  std::printf("  pipeline depth     %d\n", opts.pipeline_depth);
  std::printf("  duration           %.2f s\n", seconds);
  std::printf("  requests           %llu\n",
              static_cast<unsigned long long>(client.completed()));
  std::printf("\n");
  std::printf("  throughput         %.0f RPC/s   (%.1f MiB/s payload)\n", rps, mibps);
  std::printf("  latency p50        %.3f ms\n", base::to_millis_f(percentile(latencies, 0.50)));
  std::printf("  latency p99        %.3f ms\n", base::to_millis_f(percentile(latencies, 0.99)));
  std::printf("  latency p99.9      %.3f ms\n", base::to_millis_f(percentile(latencies, 0.999)));
  std::printf("  latency max        %.3f ms\n",
              base::to_millis_f(latencies.empty() ? 0 : latencies.back()));
  std::printf("\n");
  std::printf("  NOTE: closed-loop harness — subject to coordinated omission.\n");
  std::printf("        These are loopback smoke numbers, not publishable results.\n");
  std::printf("        Week 8 uses an open-loop generator on a real 3-node cluster.\n\n");

  return client.completed() > 0 ? 0 : 1;
}
