// logengine — one broker process.
//
// Week 6, and the point at which the architectural bet either pays or doesn't. Everything
// below is configuration and a `main()`: the broker itself is `server::Broker`, the same
// object the simulator has been crashing, partitioning and corrupting for three weeks. The
// only difference between this binary and a simulated node is which `io::` implementations
// get constructed on the next four lines.
//
//   logengine --id 0 --port 9000 --dir data/0 --peers 1@127.0.0.1:9001,2@127.0.0.1:9002
//
// With --produce it also runs a load generator, so a three-process cluster does something
// worth watching without a client library in front of it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "io/real/real_clock.h"
#include "io/real/real_disk.h"
#include "io/real/real_network.h"
#include "io/real/real_random.h"
#include "runtime/event_loop.h"
#include "bench/histogram.h"
#include "server/broker.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: logengine --id N --port P --dir PATH [options]\n"
               "  --peers LIST       id@host:port, comma separated\n"
               "  --produce          run a built-in producer while this node leads\n"
               "  --produce-ms N     milliseconds between batches (default 20)\n"
               "  --records N        records per batch (default 4)\n"
               "  --status-ms N      milliseconds between status lines (default 1000)\n"
               "  --duration-s N     exit after N seconds (default: run forever)\n"
               "  --bench-rate N     open-loop benchmark at N records/s\n"
               "  --record-bytes N   record size for the benchmark (default 1024)\n"
               "  --bind-all         listen on all interfaces, not just loopback\n"
               "                     (required for a multi-machine cluster)\n");
}

// `1@127.0.0.1:9001,2@127.0.0.1:9002`
bool parse_peers(const std::string& spec, std::vector<server::BrokerConfig::Peer>* out) {
  std::size_t start = 0;
  while (start < spec.size()) {
    const std::size_t comma = spec.find(',', start);
    const std::string item = spec.substr(start, comma == std::string::npos ? comma : comma - start);
    start = comma == std::string::npos ? spec.size() : comma + 1;
    if (item.empty()) continue;

    const std::size_t at = item.find('@');
    const std::size_t colon = item.rfind(':');
    if (at == std::string::npos || colon == std::string::npos || colon < at) return false;

    server::BrokerConfig::Peer peer;
    peer.id = static_cast<base::u32>(std::strtoul(item.substr(0, at).c_str(), nullptr, 10));
    peer.host = item.substr(at + 1, colon - at - 1);
    peer.port = static_cast<base::u16>(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 10));
    out->push_back(peer);
  }
  return true;
}

const char* role_name(const server::Broker& broker) {
  if (broker.is_leader()) return "leader";
  return broker.leader() == raft::kNoNode ? "no-leader" : "follower";
}

}  // namespace

int main(int argc, char** argv) {
  server::BrokerConfig config;
  bool produce = false;
  base::i64 produce_ms = 20;
  base::u32 records_per_batch = 4;
  base::i64 status_ms = 1000;
  base::i64 duration_s = 0;
  base::u64 bench_rate = 0;
  base::u32 record_size = 1024;
  bool have_id = false;
  bool bind_all = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--id" && has_value) {
      config.id = static_cast<base::u32>(std::strtoul(argv[++i], nullptr, 10));
      have_id = true;
    } else if (arg == "--port" && has_value) {
      config.port = static_cast<base::u16>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--dir" && has_value) {
      config.data_dir = argv[++i];
    } else if (arg == "--peers" && has_value) {
      if (!parse_peers(argv[++i], &config.peers)) {
        std::fprintf(stderr, "logengine: bad --peers, want id@host:port,...\n");
        return 2;
      }
    } else if (arg == "--produce") {
      produce = true;
    } else if (arg == "--produce-ms" && has_value) {
      produce_ms = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--records" && has_value) {
      records_per_batch = static_cast<base::u32>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--status-ms" && has_value) {
      status_ms = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--bench-rate" && has_value) {
      bench_rate = std::strtoull(argv[++i], nullptr, 10);
    } else if (arg == "--record-bytes" && has_value) {
      record_size = static_cast<base::u32>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--duration-s" && has_value) {
      duration_s = std::strtoll(argv[++i], nullptr, 10);
    } else if (arg == "--bind-all") {
      bind_all = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "logengine: unknown argument %s\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (!have_id || config.port == 0 || config.data_dir.empty()) {
    usage();
    return 2;
  }

  io::real::RealClock clock;
  io::real::RealNetwork network(bind_all);
  io::real::RealDisk disk;
  // Seeded from the OS here, from the run seed in the simulator. Same interface, and the
  // only thing Raft uses randomness for is election-timeout jitter.
  io::real::RealRandom rng;

  if (auto made = disk.make_directories(config.data_dir); !made) {
    std::fprintf(stderr, "logengine: cannot create %s: %s\n", config.data_dir.c_str(),
                 base::to_string(made.error()));
    return 1;
  }

  runtime::EventLoop loop(clock, network);
  server::Broker broker(config, loop, disk, rng, nullptr);

  if (auto started = broker.start(); !started) {
    // The one failure that must not be retried into: an unreadable `raft.state`. Coming
    // back as a fresh term-0 voter is the amnesia that elects two leaders (§13).
    std::fprintf(stderr, "logengine: node %u failed to start: %s\n", config.id,
                 base::to_string(started.error()));
    return 1;
  }

  std::fprintf(stderr, "logengine: node %u listening on :%u, dir %s, %zu peers\n",
               config.id, config.port, config.data_dir.c_str(), config.peers.size());

  storage::BatchBuilder builder;
  base::u64 proposed = 0;
  base::u64 rejected_not_leader = 0;

  // **Open-loop load, and the reason it has to be open-loop.**
  //
  // A closed-loop producer — issue, wait for the ack, issue the next — cannot report an
  // honest p99, because when the system stalls the producer stalls with it and simply
  // stops taking samples. The stall vanishes from the histogram precisely when it matters
  // most. That is coordinated omission, and it is how benchmarks accidentally report the
  // latency of a system under no load while claiming a load figure.
  //
  // So the issue schedule is fixed in advance: `next_issue` advances by the interval
  // whether or not the last batch has committed. Latency is measured from the moment a
  // record was *due to be sent*, not from the moment it actually was — a batch delayed
  // because the system was busy carries that delay in its sample, which is the whole
  // point.
  struct Inflight {
    base::u64 end_offset = 0;
    base::Nanos intended = 0;
  };
  std::vector<Inflight> inflight;
  bench::Histogram latency;
  base::u64 committed_records = 0;
  base::u64 record_bytes = 0;
  base::Nanos bench_started = 0;

  const bool bench = bench_rate > 0;
  // At `bench_rate` records/s in batches of `records_per_batch`, this is the gap between
  // batches. A batch is one Raft entry (§16.2), so this is also the entry rate.
  const base::Nanos issue_interval =
      bench ? base::Nanos(1'000'000'000LL * records_per_batch / bench_rate)
            : base::millis(produce_ms);
  base::Nanos next_issue = 0;

  auto issue_one = [&](base::Nanos intended) {
    if (!broker.is_leader() || broker.log() == nullptr) {
      ++rejected_not_leader;
      return;
    }
    const base::u64 next = broker.log()->next_offset();
    builder.clear();
    for (base::u32 i = 0; i < records_per_batch; ++i) {
      std::string payload = "r" + std::to_string(next + i) + "-";
      payload.resize(record_size, '.');
      if (!builder.add_record(base::Slice::from_string(payload))) return;
    }
    if (!broker.propose(builder)) {
      ++rejected_not_leader;
      return;
    }
    ++proposed;
    if (bench) {
      inflight.push_back(Inflight{broker.log()->next_offset(), intended});
      record_bytes += static_cast<base::u64>(records_per_batch) * record_size;
    }
  };

  const base::Nanos started_at = clock.monotonic_now();
  const base::Nanos deadline =
      duration_s > 0 ? started_at + base::seconds(duration_s) : base::kNoTimeout;
  next_issue = started_at;
  bench_started = started_at;

  base::Nanos next_status = started_at;
  while (true) {
    const base::Nanos now = clock.monotonic_now();
    if (deadline != base::kNoTimeout && now >= deadline) break;

    if (produce || bench) {
      // Catch up on every issue that was due, so a slow iteration does not silently lower
      // the offered load. Bounded so a long stall cannot turn into an unbounded burst.
      int budget = 64;
      while (next_issue <= now && budget-- > 0) {
        issue_one(next_issue);
        next_issue += issue_interval;
      }
      if (next_issue < now) next_issue = now;  // gave up catching up; do not pretend
    }

    if (bench && !inflight.empty()) {
      const base::u64 commit = broker.commit_index();
      std::size_t done = 0;
      while (done < inflight.size() && inflight[done].end_offset <= commit) {
        latency.add(now - inflight[done].intended);
        committed_records += records_per_batch;
        ++done;
      }
      if (done > 0) inflight.erase(inflight.begin(), inflight.begin() + static_cast<std::ptrdiff_t>(done));
    }

    if (status_ms > 0 && now >= next_status) {
      std::printf("node %u  %-9s term=%llu  commit=%llu  log_end=%llu  proposed=%llu\n",
                  config.id, role_name(broker),
                  static_cast<unsigned long long>(broker.term()),
                  static_cast<unsigned long long>(broker.commit_index()),
                  static_cast<unsigned long long>(broker.log_end()),
                  static_cast<unsigned long long>(proposed));
      std::fflush(stdout);
      next_status = now + base::millis(status_ms);
    }

    loop.run_once(base::millis(1));
  }

  if (bench) {
    const double seconds = base::to_seconds_f(clock.monotonic_now() - bench_started);
    const double records_per_s =
        seconds > 0 ? static_cast<double>(committed_records) / seconds : 0.0;
    const double mb_per_s =
        seconds > 0 ? static_cast<double>(record_bytes) / seconds / (1024.0 * 1024.0) : 0.0;

    std::printf("\nBENCH node=%u leader=%d\n", config.id, broker.is_leader() ? 1 : 0);
    std::printf("  offered load        %llu records/s (%u B records, %u per batch)\n",
                static_cast<unsigned long long>(bench_rate), record_size, records_per_batch);
    std::printf("  committed           %llu records in %.1f s\n",
                static_cast<unsigned long long>(committed_records), seconds);
    std::printf("  achieved            %.0f records/s, %.2f MB/s\n", records_per_s, mb_per_s);
    std::printf("  not-leader rejects  %llu\n",
                static_cast<unsigned long long>(rejected_not_leader));
    if (!latency.empty()) {
      latency.print_line("  append-ack latency", static_cast<double>(bench_rate));
    }
    std::printf("  latency is measured from when a record was *due* to be issued, not when\n");
    std::printf("  it was — an open-loop measurement, so a stall stays in the histogram.\n");
    std::fflush(stdout);
  }

  std::fprintf(stderr, "logengine: node %u stopping — term %llu, commit %llu, proposed %llu\n",
               config.id, static_cast<unsigned long long>(broker.term()),
               static_cast<unsigned long long>(broker.commit_index()),
               static_cast<unsigned long long>(proposed));
  return 0;
}
