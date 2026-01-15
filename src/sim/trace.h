#pragma once

#include <string>
#include <vector>

#include "base/types.h"

namespace sim {

// The event trace (§14.3): one line per scheduled event, `(virtual_time, node,
// event_type, key_fields)`, folded into a rolling hash.
//
// The hash is the determinism canary (I7). Determinism breaks silently — the first
// time someone iterates an `unordered_map` or reads an uninitialized byte — and the
// only reliable way to notice is to run one seed twice and compare. When it fails,
// diffing the two traces localizes the nondeterminism to a single event.
//
// Which means the trace itself must be *more* deterministic than the thing it is
// watching, so two rules apply to everything in this file:
//
//   1. Never hash a pointer. An `const char*` event name would fold the address of a
//      string literal into the hash, and addresses move between runs under ASLR — the
//      canary would then fail on every comparison and be worth nothing. Kinds are an
//      enum, hashed as a number.
//   2. Never hash a struct wholesale. `memcpy`-ing a struct into the hash reads its
//      padding bytes, which are uninitialized, which is exactly the class of bug MSAN
//      exists to find and the canary exists to catch. Fields are hashed one at a time.
enum class EventKind : base::u32 {
  kTimer = 1,

  // Storage
  kAppend = 10,       // a: base offset, b: record count
  kFsync = 11,        // a: durable bytes
  kAck = 12,          // a: base offset, b: record count
  kRecovered = 13,    // a: next offset, b: bytes truncated

  // Node lifecycle
  kCrash = 20,        // a: unflushed bytes lost
  kRestart = 21,      // a: next offset after recovery
  kCorrupt = 22,      // a: 1 if a byte was flipped

  // Network
  kConnect = 30,      // a: peer, b: conn
  kAccept = 31,       // a: peer, b: conn
  kSend = 32,         // a: conn, b: bytes
  kDeliver = 33,      // a: conn, b: bytes
  kReset = 34,        // a: conn
  kDropped = 35,      // a: conn, b: bytes discarded
  kPartitionStart = 36,  // a: from node, b: to node
  kPartitionEnd = 37,    // a: from node, b: to node
  kPing = 38,         // a: peer, b: correlation id
  kPong = 39,         // a: peer, b: correlation id
};

const char* to_string(EventKind kind) noexcept;

struct TraceEvent {
  base::Nanos time = 0;
  base::u32 node = 0;
  EventKind kind = EventKind::kTimer;
  base::u64 a = 0;
  base::u64 b = 0;
};

class Trace {
 public:
  // `recent_capacity` is what gets printed next to a failed invariant (§14.2 asks for
  // the last 50 lines). Keeping only a window is deliberate: a one-hour run produces
  // millions of events and retaining them all would trade the memory budget for
  // information nobody reads unless something failed.
  explicit Trace(std::size_t recent_capacity = 64);

  void record(const TraceEvent& event);

  [[nodiscard]] base::u64 hash() const noexcept { return hash_; }
  [[nodiscard]] base::u64 count() const noexcept { return count_; }

  // Oldest first, at most `recent_capacity` entries.
  [[nodiscard]] std::vector<TraceEvent> recent() const;

  // Full capture, for dumping a run to a file and diffing it against another. Off by
  // default and only worth turning on for a run short enough to read.
  void set_capture_all(bool on) noexcept { capture_all_ = on; }
  [[nodiscard]] const std::vector<TraceEvent>& captured() const noexcept { return all_; }

  static std::string format(const TraceEvent& event);

 private:
  base::u64 hash_;
  base::u64 count_ = 0;

  std::vector<TraceEvent> recent_;
  std::size_t next_ = 0;
  bool wrapped_ = false;

  bool capture_all_ = false;
  std::vector<TraceEvent> all_;
};

}  // namespace sim
