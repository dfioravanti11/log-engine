#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/buffer.h"
#include "base/result.h"
#include "base/types.h"
#include "io/disk.h"
#include "io/network.h"
#include "io/random.h"
#include "raft/node.h"
#include "runtime/event_loop.h"
#include "storage/log.h"
#include "wire/frame.h"

namespace server {

// One broker: a `storage::Log`, a `raft::Node`, and the wiring between them.
//
// **This class is the architectural bet, cashed in.** Weeks 3–5 built all of this inside
// `sim/`, where it ran against `io/sim/` on virtual time. Week 6 moves it here unchanged
// and runs the *same object* against `io/real/` on a real socket — because if production
// had its own copy of the driver, the simulator would be validating a sibling of the
// shipped code rather than the shipped code, and every correctness claim in the README
// would be about the wrong binary.
//
// So `Broker` holds no simulator types and no production types either. It holds `io::`
// interfaces, and which implementation arrives is the caller's business.
//
// It is also where §13 lives: `drive()` makes state durable and *then* sends, and it is
// the only function in the system that sends a Raft message.
class Broker;

// How the simulator watches without being linked into production.
//
// Everything `sim/` needs — trace events, invariant checks — is an *observation*, never a
// decision. A null observer changes nothing about what the broker does, which is what
// makes "the simulator tests the real code" true rather than aspirational.
class BrokerObserver {
 public:
  virtual ~BrokerObserver() = default;

  virtual void on_log_recovered(base::u64, base::u64) {}
  virtual void on_raft_recovered(raft::Term, raft::NodeId) {}
  virtual void on_appended(base::u64, base::u32) {}
  virtual void on_fsynced(base::u64) {}
  virtual void on_replicated(base::u64) {}
  virtual void on_truncated(base::u64) {}
  virtual void on_committed(base::u64, base::u64) {}
  virtual void on_campaign(raft::Term) {}
  virtual void on_vote(raft::Term, raft::NodeId) {}
  virtual void on_became_leader(raft::Term, base::u64) {}
  virtual void on_stepped_down(raft::Term, raft::NodeId) {}
  virtual void on_hard_state_persisted(raft::Term, raft::NodeId) {}

  // A peer sent something that cannot be true of a TCP stream carrying our own protocol.
  virtual void on_protocol_error(const char*, base::u64, base::u64) {}
};

struct BrokerConfig {
  raft::NodeId id = 0;
  std::string data_dir;
  base::u16 port = 0;

  struct Peer {
    std::string host;
    base::u16 port = 0;
    raft::NodeId id = 0;
  };
  std::vector<Peer> peers;

  // What one Raft tick is worth. Everything Raft measures is counted in these, so this is
  // the only place the state machine touches time at all (§17).
  base::Nanos tick_interval = base::millis(10);
  base::u32 election_timeout_ticks = 15;
  base::u32 election_timeout_jitter_ticks = 15;
  base::u32 heartbeat_timeout_ticks = 5;

  base::u32 segment_max_bytes = 32u * 1024u * 1024u;

  // **Both of these are faults, not features**, and both default to the safe setting.
  // They exist so the simulator can break a durability guarantee on demand and show the
  // consequence on a named seed (§13.2) — a checker nobody has watched fail is not
  // evidence. A production deployment never sets either.
  bool fsync_hard_state = true;      // false = skip the raft.state fsync
  bool ack_on_local_append = false;  // true = acks=1, ack before replicating
};

class Broker final : public io::ConnHandler {
 public:
  Broker(BrokerConfig config, runtime::EventLoop& loop, io::Disk& disk, io::Random& rng,
         BrokerObserver* observer);
  ~Broker() override;

  Broker(const Broker&) = delete;
  Broker& operator=(const Broker&) = delete;

  // Opens and recovers the log, reloads `raft.state`, rebuilds the leader epoch cache,
  // starts listening, and arms the tick timer.
  //
  // Fails, leaving the broker down, if the recorded vote is unreadable. That is
  // deliberate: coming back as a fresh term-0 voter is the amnesia that elects two
  // leaders (§13), and down is a state an operator can fix.
  base::Status start();
  void stop();

  // Appends a batch as the leader and starts replicating it. Returns the base offset
  // assigned, or kNotLeader — the caller waits for `commit_index()` to pass the offset
  // before telling a producer anything, which is what `acks=quorum` means.
  base::Result<base::u64> propose(storage::BatchBuilder& builder);

  [[nodiscard]] bool is_leader() const noexcept;
  [[nodiscard]] raft::Term term() const noexcept;
  [[nodiscard]] raft::NodeId leader() const noexcept;
  [[nodiscard]] base::u64 commit_index() const noexcept;
  [[nodiscard]] base::u64 log_end() const noexcept;
  [[nodiscard]] storage::Log* log() noexcept { return log_.get(); }
  [[nodiscard]] const BrokerConfig& config() const noexcept { return config_; }

  [[nodiscard]] base::u64 ticks() const noexcept { return ticks_; }
  [[nodiscard]] base::u64 appends() const noexcept { return appends_; }
  [[nodiscard]] base::u64 raft_messages_sent() const noexcept { return raft_sent_; }
  [[nodiscard]] base::u64 hard_state_writes() const noexcept { return hard_state_writes_; }

  void on_readable(io::ConnId conn) override;
  void on_writable(io::ConnId conn) override;
  void on_hangup(io::ConnId conn) override;

 private:
  struct Stream {
    base::Buffer in;
    base::Buffer out;
    wire::FrameDecoder decoder;
    bool outgoing = false;
  };

  base::Status load_hard_state(raft::HardState* out);
  void schedule_tick();
  void on_tick();

  // Persist, then send. The whole of §13, in one place.
  void drive(base::Slice entry);
  base::Status persist(const raft::HardState& state);
  base::Status carry_out_log_orders(const raft::Ready& ready, base::Slice entry);
  void note_commit(base::u64 commit_index);
  base::Result<base::Slice> read_entry(base::u64 index);

  void receive_raft(base::Slice payload);
  void deliver_locally(const raft::Message& message, base::Slice entry);
  void send_raft(const raft::Message& message);
  void note_transition(raft::Role role_before, raft::Term term_before,
                       raft::NodeId vote_before);

  void send(io::ConnId conn, const wire::FrameHeader& header, base::Slice payload);
  void flush(io::ConnId conn);
  void forget(io::ConnId conn);

  BrokerConfig config_;
  runtime::EventLoop& loop_;
  io::Disk& disk_;
  io::Random& rng_;
  BrokerObserver* observer_;

  std::unique_ptr<storage::Log> log_;
  std::unique_ptr<raft::Node> raft_;
  io::FileId state_file_ = io::kInvalidFile;
  base::u64 state_sequence_ = 0;
  base::u64 last_commit_seen_ = 0;
  std::vector<base::u8> entry_buf_;

  io::ConnId listener_ = io::kInvalidConn;
  std::map<io::ConnId, Stream> streams_;
  std::vector<io::ConnId> peer_conns_;

  base::u32 next_correlation_ = 1;
  base::u64 ticks_ = 0;
  base::u64 appends_ = 0;
  base::u64 raft_sent_ = 0;
  base::u64 raft_received_ = 0;
  base::u64 hard_state_writes_ = 0;
  bool running_ = false;
};

}  // namespace server
