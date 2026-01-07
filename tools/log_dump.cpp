// log-dump — print batch headers from a segment file, validating every CRC.
//
// Read-only, deliberately. The obvious implementation opens the segment through
// storage::Segment and prints what it finds, but Segment::open() *recovers*: it
// truncates the torn tail as a side effect. A forensic tool that destroys the evidence
// on the way to displaying it is worse than no tool, so this walks the bytes itself and
// reports what it sees, including the parts recovery would throw away.
//
// Used daily from week 2 to week 5 (§14.4), which is why it exists this early.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "io/real/real_disk.h"
#include "storage/record_batch.h"

namespace {

using base::Slice;

void usage() {
  std::fprintf(stderr,
               "usage: log-dump <segment.log> [--records] [--quiet]\n"
               "  --records  print each record's length and a printable prefix\n"
               "  --quiet    summary only\n");
}

// Records are arbitrary bytes; a terminal is not. Print what is printable and escape
// the rest rather than letting a payload reprogram the reader's terminal.
std::string printable(Slice value, std::size_t max_chars) {
  std::string out;
  for (std::size_t i = 0; i < value.size() && out.size() < max_chars; ++i) {
    const base::u8 c = value[i];
    if (c >= 0x20 && c < 0x7F) {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out.append(buf);
    }
  }
  if (value.size() > max_chars) out.append("...");
  return out;
}

struct Summary {
  base::u64 batches = 0;
  base::u64 records = 0;
  base::u64 control_batches = 0;
  base::u64 first_offset = 0;
  base::u64 next_offset = 0;
  base::u64 valid_bytes = 0;
  base::u64 trailing_bytes = 0;
  const char* stop_reason = nullptr;
};

int dump(const std::string& path, bool show_records, bool quiet) {
  io::real::RealDisk disk;
  auto file = disk.open(path, io::OpenMode::kRead);
  if (!file) {
    std::fprintf(stderr, "log-dump: cannot open %s (%s)\n", path.c_str(),
                 base::to_string(file.error()));
    return 1;
  }

  auto size = disk.size(file.value());
  if (!size) {
    std::fprintf(stderr, "log-dump: cannot stat %s\n", path.c_str());
    return 1;
  }

  std::vector<base::u8> bytes(static_cast<std::size_t>(size.value()));
  auto got = disk.pread(file.value(), base::MutSlice(bytes.data(), bytes.size()), 0);
  if (!got) {
    std::fprintf(stderr, "log-dump: cannot read %s\n", path.c_str());
    return 1;
  }
  bytes.resize(got.value());
  disk.close(file.value());

  std::printf("%s  (%zu bytes)\n\n", path.c_str(), bytes.size());
  if (!quiet) {
    std::printf("%10s %12s %12s %7s %6s %6s %8s %10s  %s\n", "pos", "base_offset",
                "last_offset", "records", "epoch", "attrs", "bytes", "crc", "status");
  }

  Summary sum;
  std::size_t pos = 0;
  base::u32 last_epoch = 0;
  bool have_epoch = false;

  while (pos < bytes.size()) {
    const Slice framed = Slice(bytes.data(), bytes.size()).subslice(pos);

    auto header = storage::decode_header(framed);
    if (!header) {
      sum.stop_reason = bytes.size() - pos < storage::kBatchHeaderBytes
                            ? "short tail: fewer bytes than a batch header"
                            : "undecodable batch header";
      break;
    }
    const storage::BatchHeader& h = header.value();

    if (h.total_bytes() > bytes.size() - pos) {
      sum.stop_reason = "torn write: batch claims more bytes than the file holds";
      break;
    }

    const auto valid = storage::validate_batch(framed);
    const char* status = valid ? "ok" : "CORRUPT";

    // An epoch boundary is where log divergence gets resolved (§12.3), so it is worth
    // seeing at a glance rather than inferring from a column.
    if (!quiet && (!have_epoch || h.leader_epoch != last_epoch)) {
      std::printf("--- leader epoch %u begins at offset %llu\n", h.leader_epoch,
                  static_cast<unsigned long long>(h.base_offset));
    }
    have_epoch = true;
    last_epoch = h.leader_epoch;

    if (!quiet) {
      std::printf("%10zu %12llu %12llu %7u %6u %6s %8zu 0x%08X  %s\n", pos,
                  static_cast<unsigned long long>(h.base_offset),
                  static_cast<unsigned long long>(h.last_offset()), h.record_count,
                  h.leader_epoch, h.is_control() ? "ctrl" : "-", h.total_bytes(), h.crc,
                  status);
    }

    if (!valid) {
      sum.stop_reason = "CRC or structure check failed";
      break;
    }

    if (show_records) {
      storage::RecordIterator it(framed);
      Slice value;
      base::u64 offset = h.base_offset;
      while (it.next(&value)) {
        std::printf("           offset %llu  %zu bytes  %s\n",
                    static_cast<unsigned long long>(offset), value.size(),
                    printable(value, 48).c_str());
        ++offset;
      }
    }

    if (sum.batches == 0) sum.first_offset = h.base_offset;
    ++sum.batches;
    sum.records += h.record_count;
    if (h.is_control()) ++sum.control_batches;
    sum.next_offset = h.next_offset();
    pos += h.total_bytes();
    sum.valid_bytes = pos;
  }

  sum.trailing_bytes = bytes.size() - sum.valid_bytes;

  std::printf("\nsummary\n");
  std::printf("  batches         %llu (%llu control)\n",
              static_cast<unsigned long long>(sum.batches),
              static_cast<unsigned long long>(sum.control_batches));
  std::printf("  records         %llu\n", static_cast<unsigned long long>(sum.records));
  if (sum.batches > 0) {
    // Offsets are monotonic, not dense (FR-6) — the record count and the offset span
    // are printed separately on purpose, because control batches make them differ.
    std::printf("  offsets         %llu .. %llu (next: %llu)\n",
                static_cast<unsigned long long>(sum.first_offset),
                static_cast<unsigned long long>(sum.next_offset - 1),
                static_cast<unsigned long long>(sum.next_offset));
  }
  std::printf("  valid bytes     %llu of %zu\n",
              static_cast<unsigned long long>(sum.valid_bytes), bytes.size());
  if (sum.trailing_bytes > 0) {
    std::printf("  unreadable tail %llu bytes — %s\n",
                static_cast<unsigned long long>(sum.trailing_bytes), sum.stop_reason);
    std::printf("  (recovery would truncate here on the next open; a torn tail is normal)\n");
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  bool show_records = false;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--records") {
      show_records = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "log-dump: unknown option %s\n", arg.c_str());
      usage();
      return 2;
    } else {
      path = arg;
    }
  }

  if (path.empty()) {
    usage();
    return 2;
  }
  return dump(path, show_records, quiet);
}
