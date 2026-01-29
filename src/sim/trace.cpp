#include "sim/trace.h"

#include <cstdio>

namespace sim {
namespace {

// FNV-1a, 64-bit. Not a cryptographic choice and doesn't need to be: the trace hash
// has to detect accidental divergence between two runs of the same seed, not resist
// an adversary constructing a collision. It is fed one byte at a time so the result
// does not depend on the host's endianness — a hash that differed between an x86 CI
// runner and an ARM laptop would make the canary useless in exactly the place it is
// needed most.
constexpr base::u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr base::u64 kFnvPrime = 1099511628211ull;

base::u64 mix(base::u64 hash, base::u64 value) noexcept {
  for (int byte = 0; byte < 8; ++byte) {
    hash ^= (value >> (byte * 8)) & 0xFFull;
    hash *= kFnvPrime;
  }
  return hash;
}

}  // namespace

const char* to_string(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::kTimer: return "TIMER";
    case EventKind::kAppend: return "APPEND";
    case EventKind::kFsync: return "FSYNC";
    case EventKind::kAck: return "ACK";
    case EventKind::kRecovered: return "RECOVERED";
    case EventKind::kCrash: return "CRASH";
    case EventKind::kRestart: return "RESTART";
    case EventKind::kCorrupt: return "CORRUPT";
    case EventKind::kConnect: return "CONNECT";
    case EventKind::kAccept: return "ACCEPT";
    case EventKind::kSend: return "SEND";
    case EventKind::kDeliver: return "DELIVER";
    case EventKind::kReset: return "RESET";
    case EventKind::kDropped: return "DROPPED";
    case EventKind::kPartitionStart: return "PARTITION_START";
    case EventKind::kPartitionEnd: return "PARTITION_END";
    case EventKind::kCampaign: return "CAMPAIGN";
    case EventKind::kVote: return "VOTE";
    case EventKind::kLeader: return "LEADER";
    case EventKind::kStepDown: return "STEP_DOWN";
    case EventKind::kRaftPersist: return "RAFT_PERSIST";
    case EventKind::kRaftRecover: return "RAFT_RECOVER";
    case EventKind::kReplicate: return "REPLICATE";
    case EventKind::kTruncate: return "TRUNCATE";
    case EventKind::kCommit: return "COMMIT";
  }
  return "UNKNOWN";
}

Trace::Trace(std::size_t recent_capacity) : hash_(kFnvOffsetBasis) {
  recent_.resize(recent_capacity == 0 ? 1 : recent_capacity);
}

void Trace::record(const TraceEvent& event) {
  hash_ = mix(hash_, static_cast<base::u64>(event.time));
  hash_ = mix(hash_, event.node);
  hash_ = mix(hash_, static_cast<base::u64>(event.kind));
  hash_ = mix(hash_, event.a);
  hash_ = mix(hash_, event.b);
  ++count_;

  recent_[next_] = event;
  next_ = (next_ + 1) % recent_.size();
  if (next_ == 0) wrapped_ = true;

  if (capture_all_) all_.push_back(event);
}

std::vector<TraceEvent> Trace::recent() const {
  std::vector<TraceEvent> out;
  if (!wrapped_) {
    out.assign(recent_.begin(), recent_.begin() + static_cast<std::ptrdiff_t>(next_));
    return out;
  }
  out.reserve(recent_.size());
  for (std::size_t i = 0; i < recent_.size(); ++i) {
    out.push_back(recent_[(next_ + i) % recent_.size()]);
  }
  return out;
}

std::string Trace::format(const TraceEvent& event) {
  char buf[128];
  // Fixed width, one line per event, no floating point: a trace meant to be diffed
  // must not have fields that wander between runs or round differently.
  const int n = std::snprintf(buf, sizeof(buf), "%14lld %3u %-16s %20llu %20llu",
                              static_cast<long long>(event.time), event.node,
                              to_string(event.kind),
                              static_cast<unsigned long long>(event.a),
                              static_cast<unsigned long long>(event.b));
  return std::string(buf, n < 0 ? 0 : static_cast<std::size_t>(n));
}

}  // namespace sim
