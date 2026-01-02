#pragma once

#include "base/types.h"

namespace wire {

// Every API is versioned from v0 (§16.3). It costs nothing now and it is the
// difference between "thought about compatibility" and "will rewrite the protocol
// the first time a field is added".
enum class ApiKey : base::u16 {
  kProduce = 0,
  kFetch = 1,
  kMetadata = 2,
  kListOffsets = 3,
  kOffsetCommit = 4,

  // Broker-to-broker. Numbered from 100 so a client key and a Raft key can never be
  // confused by a misrouted frame.
  kRequestVote = 100,
  kAppendEntries = 101,
  kInstallSnapshot = 102,

  // Not part of the protocol: used only by bench/echo to exercise the transport.
  kEcho = 1000,
};

inline constexpr base::u16 kApiVersion0 = 0;

constexpr bool is_broker_api(ApiKey key) {
  const auto v = static_cast<base::u16>(key);
  return v >= 100 && v < 1000;
}

constexpr const char* to_string(ApiKey key) {
  switch (key) {
    case ApiKey::kProduce: return "Produce";
    case ApiKey::kFetch: return "Fetch";
    case ApiKey::kMetadata: return "Metadata";
    case ApiKey::kListOffsets: return "ListOffsets";
    case ApiKey::kOffsetCommit: return "OffsetCommit";
    case ApiKey::kRequestVote: return "RequestVote";
    case ApiKey::kAppendEntries: return "AppendEntries";
    case ApiKey::kInstallSnapshot: return "InstallSnapshot";
    case ApiKey::kEcho: return "Echo";
  }
  return "Unknown";
}

}  // namespace wire
