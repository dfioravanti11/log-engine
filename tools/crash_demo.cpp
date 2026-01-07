// crash-demo — the week 2 demo, in two halves.
//
//   crash-demo append --dir D --acks F    appends forever, fsyncing before it acks
//   crash-demo verify --dir D --acks F    reopens the log and checks every ack held
//
// The point is the definition of "acked". The appender writes a batch, fsyncs the log,
// and only then records the offset in the ack file (which it also fsyncs). That
// ordering is the whole contract: if the process dies anywhere in between, the worst
// case is durable data nobody was promised — never a promise without the data. Extra
// records past the last ack are expected and fine; a missing acked record is a failed
// demo, and the verifier exits non-zero.
//
// `kill -9` at any moment must leave verify green. See scripts/demo_week2.sh.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "io/real/real_disk.h"
#include "storage/log.h"
#include "storage/record_batch.h"

namespace {

using base::Slice;

// Content is a pure function of the offset, so the verifier recomputes what it should
// see instead of trusting a second copy of the data.
std::string payload_for(base::u64 offset) {
  std::string s = "record-" + std::to_string(offset) + "-";
  s.resize(64, '.');
  return s;
}

struct Ack {
  base::u64 base_offset = 0;
  base::u64 count = 0;
};

// The ack file is the producer's memory of what it was promised. It is a plain text
// list so a human can read it mid-demo, and it is fsynced after every line so its own
// tail is never the thing that loses the record.
class AckFile {
 public:
  base::Status open_for_append(io::Disk& disk, const std::string& path) {
    disk_ = &disk;
    auto file = disk.open(path, io::OpenMode::kCreate);
    if (!file) return base::fail(file.error());
    id_ = file.value();
    auto size = disk.size(id_);
    if (!size) return base::fail(size.error());
    pos_ = size.value();
    return {};
  }

  base::Status record(base::u64 base_offset, base::u64 count) {
    const std::string line =
        std::to_string(base_offset) + " " + std::to_string(count) + "\n";
    auto written = disk_->pwrite(id_, Slice::from_string(line), pos_);
    if (!written) return base::fail(written.error());
    pos_ += written.value();
    return disk_->fsync(id_);
  }

  static std::vector<Ack> read_all(io::Disk& disk, const std::string& path) {
    std::vector<Ack> acks;
    auto file = disk.open(path, io::OpenMode::kRead);
    if (!file) return acks;

    auto size = disk.size(file.value());
    if (!size) return acks;
    std::vector<base::u8> bytes(static_cast<std::size_t>(size.value()));
    auto got = disk.pread(file.value(), base::MutSlice(bytes.data(), bytes.size()), 0);
    disk.close(file.value());
    if (!got) return acks;

    // A line without its newline was still being written when the process died, and
    // an ack that was never durable was never an ack. Drop it.
    std::string text(reinterpret_cast<const char*>(bytes.data()), got.value());
    std::size_t start = 0;
    while (true) {
      const std::size_t nl = text.find('\n', start);
      if (nl == std::string::npos) break;
      const std::string line = text.substr(start, nl - start);
      start = nl + 1;

      const std::size_t space = line.find(' ');
      if (space == std::string::npos) continue;
      acks.push_back(Ack{std::strtoull(line.c_str(), nullptr, 10),
                         std::strtoull(line.c_str() + space + 1, nullptr, 10)});
    }
    return acks;
  }

 private:
  io::Disk* disk_ = nullptr;
  io::FileId id_ = io::kInvalidFile;
  base::u64 pos_ = 0;
};

int run_append(const std::string& dir, const std::string& acks_path, base::u32 records_per_batch,
               base::u64 max_batches) {
  io::real::RealDisk disk;

  storage::Log::Options options;
  options.segment_max_bytes = 64 * 1024;  // small, so the demo rolls segments in seconds

  auto opened = storage::Log::open(disk, dir, options);
  if (!opened) {
    std::fprintf(stderr, "append: cannot open log (%s)\n", base::to_string(opened.error()));
    return 1;
  }
  std::unique_ptr<storage::Log> log = std::move(opened).value();

  AckFile acks;
  if (auto st = acks.open_for_append(disk, acks_path); !st) {
    std::fprintf(stderr, "append: cannot open ack file (%s)\n", base::to_string(st.error()));
    return 1;
  }

  std::fprintf(stderr, "append: writing from offset %llu, %u records/batch, fsync per batch\n",
               static_cast<unsigned long long>(log->next_offset()), records_per_batch);

  storage::BatchBuilder builder;
  for (base::u64 batch = 0; max_batches == 0 || batch < max_batches; ++batch) {
    const base::u64 base_offset = log->next_offset();

    builder.clear();
    for (base::u32 i = 0; i < records_per_batch; ++i) {
      const std::string value = payload_for(base_offset + i);
      if (auto st = builder.add_record(Slice::from_string(value)); !st) return 1;
    }

    auto assigned = log->append(builder, storage::BatchMeta{});
    if (!assigned) {
      std::fprintf(stderr, "append: %s\n", base::to_string(assigned.error()));
      return 1;
    }
    if (auto st = log->fsync(); !st) {
      std::fprintf(stderr, "append: fsync failed (%s)\n", base::to_string(st.error()));
      return 1;
    }
    // Only now is it acked. Everything before this line is data the producer was
    // never promised.
    if (auto st = acks.record(assigned.value(), records_per_batch); !st) {
      std::fprintf(stderr, "append: ack write failed (%s)\n", base::to_string(st.error()));
      return 1;
    }

    if (batch % 200 == 0) {
      std::fprintf(stderr, "append: %llu batches acked, next offset %llu\n",
                   static_cast<unsigned long long>(batch + 1),
                   static_cast<unsigned long long>(log->next_offset()));
    }
  }
  return 0;
}

int run_verify(const std::string& dir, const std::string& acks_path) {
  io::real::RealDisk disk;

  const std::vector<Ack> acks = AckFile::read_all(disk, acks_path);
  if (acks.empty()) {
    std::fprintf(stderr, "verify: no acks recorded — the demo proved nothing\n");
    return 1;
  }

  storage::Log::Options options;
  options.segment_max_bytes = 64 * 1024;

  auto opened = storage::Log::open(disk, dir, options);
  if (!opened) {
    std::fprintf(stderr, "verify: cannot open log (%s)\n", base::to_string(opened.error()));
    return 1;
  }
  std::unique_ptr<storage::Log> log = std::move(opened).value();

  const storage::LogRecoveryReport& r = log->recovery();
  std::printf("recovery: %llu segments, %llu batches scanned, %llu bytes truncated%s\n",
              static_cast<unsigned long long>(r.segments_opened),
              static_cast<unsigned long long>(r.batches_scanned),
              static_cast<unsigned long long>(r.bytes_truncated),
              r.truncated ? " (torn tail dropped — this is the normal case)" : "");
  std::printf("log:      offsets %llu .. %llu\n",
              static_cast<unsigned long long>(log->start_offset()),
              static_cast<unsigned long long>(log->next_offset()));

  // Summed, not derived from the last ack. The acked set is not necessarily
  // contiguous: a crash can leave durable-but-unacked records behind, and the next run
  // starts appending after them, so the ack list legitimately contains a gap.
  base::u64 acked_records = 0;
  for (const Ack& ack : acks) acked_records += ack.count;
  const base::u64 acked_end = acks.back().base_offset + acks.back().count;

  std::printf("acks:     %zu batches, %llu records, highest acked offset %llu\n", acks.size(),
              static_cast<unsigned long long>(acked_records),
              static_cast<unsigned long long>(acked_end - 1));

  std::vector<base::u8> buf(256 * 1024);
  base::u64 checked = 0;
  base::u64 missing = 0;
  base::u64 mismatched = 0;

  for (const Ack& ack : acks) {
    if (ack.base_offset + ack.count > log->next_offset()) {
      std::printf("LOST: acked offsets %llu..%llu are not in the log (log ends at %llu)\n",
                  static_cast<unsigned long long>(ack.base_offset),
                  static_cast<unsigned long long>(ack.base_offset + ack.count - 1),
                  static_cast<unsigned long long>(log->next_offset()));
      missing += ack.count;
      continue;
    }

    auto got = log->read(ack.base_offset, base::MutSlice(buf.data(), buf.size()));
    if (!got || got.value() == 0) {
      std::printf("LOST: acked offset %llu unreadable\n",
                  static_cast<unsigned long long>(ack.base_offset));
      missing += ack.count;
      continue;
    }

    const Slice framed = Slice(buf.data(), got.value());
    auto header = storage::decode_header(framed);
    if (!header || header.value().base_offset != ack.base_offset) {
      std::printf("MISMATCH: read at acked offset %llu returned the wrong batch\n",
                  static_cast<unsigned long long>(ack.base_offset));
      ++mismatched;
      continue;
    }

    storage::RecordIterator it(framed);
    Slice value;
    base::u64 offset = ack.base_offset;
    while (it.next(&value)) {
      if (value != Slice::from_string(payload_for(offset))) {
        std::printf("MISMATCH: offset %llu holds bytes that were never written\n",
                    static_cast<unsigned long long>(offset));
        ++mismatched;
      }
      ++offset;
      ++checked;
    }
  }

  const base::u64 extra =
      log->next_offset() > acked_end ? log->next_offset() - acked_end : 0;
  std::printf("checked:  %llu acked records\n", static_cast<unsigned long long>(checked));
  if (extra > 0) {
    // Durable but unacked. Expected, and the right direction to be wrong in.
    std::printf("extra:    %llu durable records past the last ack — fine, nobody was promised them\n",
                static_cast<unsigned long long>(extra));
  }

  if (missing > 0 || mismatched > 0) {
    std::printf("\nFAILED: %llu acked records lost, %llu mismatched\n",
                static_cast<unsigned long long>(missing),
                static_cast<unsigned long long>(mismatched));
    return 1;
  }
  std::printf("\nOK: every acked record survived the crash\n");
  return 0;
}

void usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  crash-demo append --dir D --acks F [--records-per-batch N] [--batches N]\n"
               "  crash-demo verify --dir D --acks F\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }

  const std::string mode = argv[1];
  std::string dir;
  std::string acks;
  base::u32 records_per_batch = 4;
  base::u64 batches = 0;  // 0 = until killed, which is the point

  for (int i = 2; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--dir") {
      dir = value;
    } else if (key == "--acks") {
      acks = value;
    } else if (key == "--records-per-batch") {
      records_per_batch = static_cast<base::u32>(std::strtoul(value.c_str(), nullptr, 10));
    } else if (key == "--batches") {
      batches = std::strtoull(value.c_str(), nullptr, 10);
    } else {
      std::fprintf(stderr, "crash-demo: unknown option %s\n", key.c_str());
      usage();
      return 2;
    }
  }

  if (dir.empty() || acks.empty() || records_per_batch == 0) {
    usage();
    return 2;
  }

  if (mode == "append") return run_append(dir, acks, records_per_batch, batches);
  if (mode == "verify") return run_verify(dir, acks);

  usage();
  return 2;
}
