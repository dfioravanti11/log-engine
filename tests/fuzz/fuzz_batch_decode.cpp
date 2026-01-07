// libFuzzer target for the batch decoder — the parser that reads bytes off a disk
// that may have lied to us.
//
// The contract being fuzzed is narrow and absolute: for *any* input, decode_header(),
// validate_batch(), and RecordIterator must return an answer without reading outside
// the buffer, looping forever, or allocating on a length field the input controls.
// A four-byte length an attacker picks is the cheapest OOM in the world (§16.3), and
// the recovery scan feeds this exact function with whatever survived a power cut.
//
// Build: cmake --preset fuzz && cmake --build --preset fuzz
// Run:   ./build/fuzz/tests/fuzz_batch_decode -max_total_time=60 tests/fuzz/corpus/batch

#include <cstddef>
#include <cstdint>

#include "storage/record_batch.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const base::Slice input(data, size);

  auto header = storage::decode_header(input);
  if (header) {
    // decode_header having succeeded means total_bytes() is already bounded by
    // kMaxBatchBytes. Anything that walks the batch must still respect the *actual*
    // buffer, which is usually shorter.
    (void)header.value().total_bytes();
    (void)header.value().next_offset();
  }

  const bool valid = storage::validate_batch(input).ok();

  // The iterator is documented as taking a validated batch, but recovery and log-dump
  // both reach it after a decode that could have been fooled, so it has to be safe on
  // garbage too.
  storage::RecordIterator it(input);
  base::Slice value;
  std::size_t records = 0;
  while (it.next(&value)) {
    ++records;
    if (records > storage::kMaxBatchBytes) __builtin_trap();  // an iterator that never ends
  }

  // A batch that validates must expose exactly the record count its header claims.
  // This is the assertion that turns "did not crash" into "was actually correct".
  if (valid && records != header.value().record_count) __builtin_trap();

  return 0;
}
