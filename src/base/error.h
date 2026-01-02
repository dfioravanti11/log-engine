#pragma once

#include "base/types.h"

namespace base {

// Wire-visible error codes (§8.3). These travel as a u16 *per partition* inside a
// response, never per connection: one bad partition must not fail a batched request.
//
// The retryable/terminal split is load-bearing — the client library retries the
// first class with jittered backoff and surfaces the second to the caller. Adding a
// code means deciding which class it is; there is no third option.
enum class ErrorCode : u16 {
  kNone = 0,

  // ---- Retryable: the request may succeed if sent again. ----
  kNotLeader = 1,          // leader hint accompanies this in the response
  kRequestTimedOut = 2,
  kNotEnoughReplicas = 3,
  kWouldBlock = 4,         // internal only: non-blocking I/O has nothing right now

  // ---- Terminal: retrying will fail the same way. ----
  kOffsetOutOfRange = 100,
  kCorruptRecord = 101,
  kInvalidProducerEpoch = 102,
  kMessageTooLarge = 103,
  kInvalidRequest = 104,
  kUnsupportedVersion = 105,
  kIoError = 106,
  kClosed = 107,
  kNotFound = 108,
  kInvalidArgument = 109,
};

constexpr bool is_retryable(ErrorCode code) noexcept {
  return static_cast<u16>(code) > 0 && static_cast<u16>(code) < 100;
}

constexpr const char* to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kNone: return "NONE";
    case ErrorCode::kNotLeader: return "NOT_LEADER";
    case ErrorCode::kRequestTimedOut: return "REQUEST_TIMED_OUT";
    case ErrorCode::kNotEnoughReplicas: return "NOT_ENOUGH_REPLICAS";
    case ErrorCode::kWouldBlock: return "WOULD_BLOCK";
    case ErrorCode::kOffsetOutOfRange: return "OFFSET_OUT_OF_RANGE";
    case ErrorCode::kCorruptRecord: return "CORRUPT_RECORD";
    case ErrorCode::kInvalidProducerEpoch: return "INVALID_PRODUCER_EPOCH";
    case ErrorCode::kMessageTooLarge: return "MESSAGE_TOO_LARGE";
    case ErrorCode::kInvalidRequest: return "INVALID_REQUEST";
    case ErrorCode::kUnsupportedVersion: return "UNSUPPORTED_VERSION";
    case ErrorCode::kIoError: return "IO_ERROR";
    case ErrorCode::kClosed: return "CLOSED";
    case ErrorCode::kNotFound: return "NOT_FOUND";
    case ErrorCode::kInvalidArgument: return "INVALID_ARGUMENT";
  }
  return "UNKNOWN";
}

}  // namespace base
