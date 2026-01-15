#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/buffer.h"
#include "io/disk.h"
#include "io/network.h"
#include "io/random.h"
#include "runtime/event_loop.h"
#include "sim/scheduler.h"
#include "storage/log.h"
#include "wire/frame.h"

namespace sim {

// One batch the cluster promised a producer: appended *and* fsynced before the ack.
struct AckedBatch {
  base::u64 base_offset = 0;
  base::u32 record_count = 0;
};

// The shadow model (§14.2). It lives in the simulator, never in a node, and that is the
// whole point: a node checking its own durability against its own memory would agree
// with itself after losing both. The oracle remembers what was promised across crashes
// that the node does not survive.
class Oracle {
 public:
  explicit Oracle(base::u32 node_count) : acked_(node_count) {}

  void record_ack(base::u32 node, AckedBatch batch) { acked_[node].push_back(batch); }
  [[nodiscard]] const std::vector<AckedBatch>& acks(base::u32 node) const {
    return acked_[node];
  }
  [[nodiscard]] base::u64 total_batches() const;

  // Offset one past the highest acked record, or 0 if nothing has been acked.
  [[nodiscard]] base::u64 acked_end(base::u32 node) const;
  [[nodiscard]] base::u64 total_records() const;

  // First violation wins: everything after the first broken invariant is a consequence,
  // and reporting the cascade buries the cause.
  void violation(const char* invariant, std::string detail);
  [[nodiscard]] bool ok() const noexcept { return invariant_ == nullptr; }
  [[nodiscard]] const char* invariant() const noexcept { return invariant_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

 private:
  std::vector<std::vector<AckedBatch>> acked_;
  const char* invariant_ = nullptr;
  std::string detail_;
};

// Deterministic record contents, so the verifier recomputes what it should see instead
// of trusting a second copy of the data.
std::string payload_for(base::u32 node, base::u64 offset);

// What each simulated node actually does: append to a real storage::Log through the
// simulated disk, and exchange framed ping/pong with its peers through the simulated
// network.
//
// There is no Raft here yet — that is weeks 4–5. What this exercises is everything
// underneath it: the durability contract under power loss, the recovery path against
// the one fault `kill -9` provably cannot produce (`docs/retrospective.md` §5), and the
// week-1 wire codec against partial writes, backpressure, and partitions.
//
// The object is destroyed and rebuilt on every crash, exactly like the process it
// stands in for. The disk it writes to is not.
class NodeWorkload final : public io::ConnHandler {
 public:
  struct Config {
    base::Nanos append_interval = base::millis(40);
    base::Nanos append_jitter = base::millis(20);
    base::Nanos ping_interval = base::millis(250);
    base::u32 records_per_batch = 2;
    base::u32 record_bytes = 48;
    // Recovery yields a prefix, so checking the tail proves the rest. See verify().
    std::size_t verify_tail_batches = 32;
  };

  struct Peer {
    std::string host;
    base::u16 port = 0;
  };

  NodeWorkload(base::u32 node, Scheduler& scheduler, runtime::EventLoop& loop,
               io::Disk& disk, io::Random& rng, Oracle& oracle, std::string dir,
               base::u16 port, std::vector<Peer> peers, Config config);
  ~NodeWorkload() override;

  // Opens the log — which recovers it — then checks the oracle's promises survived.
  base::Status start();

  void on_readable(io::ConnId conn) override;
  void on_writable(io::ConnId conn) override;
  void on_hangup(io::ConnId conn) override;

  [[nodiscard]] base::u64 appends() const noexcept { return appends_; }
  [[nodiscard]] base::u64 pongs() const noexcept { return pongs_; }

 private:
  struct Stream {
    base::Buffer in;
    base::Buffer out;
    wire::FrameDecoder decoder;
    bool outgoing = false;
    std::set<base::u32> awaiting;  // correlation ids this side is still expecting
  };

  void verify_recovery();
  void schedule_append();
  void do_append();
  void schedule_ping();
  void do_ping();
  void serve(io::ConnId conn, const wire::FrameHeader& header, base::Slice payload);
  void receive_pong(io::ConnId conn, const wire::FrameHeader& header);
  void send(io::ConnId conn, const wire::FrameHeader& header, base::Slice payload);
  void flush(io::ConnId conn);
  void forget(io::ConnId conn);

  base::u32 node_;
  Scheduler& scheduler_;
  runtime::EventLoop& loop_;
  io::Disk& disk_;
  io::Random& rng_;
  Oracle& oracle_;
  std::string dir_;
  base::u16 port_;
  std::vector<Peer> peers_;
  Config config_;

  std::unique_ptr<storage::Log> log_;
  storage::BatchBuilder builder_;

  io::ConnId listener_ = io::kInvalidConn;
  std::map<io::ConnId, Stream> streams_;
  std::vector<io::ConnId> peer_conns_;  // one per entry in peers_, or kInvalidConn

  base::u32 next_correlation_ = 1;
  base::u64 appends_ = 0;
  base::u64 pongs_ = 0;
  bool running_ = false;
};

}  // namespace sim
