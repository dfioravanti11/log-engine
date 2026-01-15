// sim — run the deterministic fault simulator.
//
//   ./sim --seeds 100                 run 100 seeds, exit non-zero on any violation
//   ./sim --seed 0x3f2a91c4           run one seed and print its trace hash
//   ./sim --seed 7 --dump-trace out   write the full event trace, for diffing
//
// Never prints a result without its seed (CLAUDE.md rule 5). A lost seed is a lost bug.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sim/simulation.h"

namespace {

void usage() {
  std::fprintf(stderr,
               "usage: sim [options]\n"
               "  --seeds N          run seeds 1..N (default 10)\n"
               "  --seed X           run exactly this seed\n"
               "  --nodes N          nodes in the cluster (default 3)\n"
               "  --duration-s N     simulated seconds per run (default 60)\n"
               "  --dump-trace PATH  write the full event trace of a single seed\n"
               "  --io-errors P      per-operation disk I/O error probability\n"
               "  --no-faults        no crashes, partitions, or clock jumps\n"
               "  --quiet            one line per seed\n");
}

void print_result(const sim::SimulationResult& result, bool quiet) {
  if (quiet) {
    std::printf("seed %-20llu hash %016llx  events %-10llu %s\n",
                static_cast<unsigned long long>(result.seed),
                static_cast<unsigned long long>(result.trace_hash),
                static_cast<unsigned long long>(result.trace_events),
                result.ok ? "ok" : "VIOLATION");
    return;
  }

  std::printf("seed                %llu (0x%llx)\n",
              static_cast<unsigned long long>(result.seed),
              static_cast<unsigned long long>(result.seed));
  std::printf("trace hash          %016llx over %llu events\n",
              static_cast<unsigned long long>(result.trace_hash),
              static_cast<unsigned long long>(result.trace_events));
  std::printf("simulated           %.1f s of %u nodes = %.3f node-hours\n",
              base::to_seconds_f(result.simulated_time), result.node_count,
              result.simulated_node_hours());
  std::printf("acked               %llu records over %llu batches\n",
              static_cast<unsigned long long>(result.records_acked),
              static_cast<unsigned long long>(result.appends));
  std::printf("faults              %llu crashes, %llu partitions, %llu resets\n",
              static_cast<unsigned long long>(result.crashes),
              static_cast<unsigned long long>(result.partitions),
              static_cast<unsigned long long>(result.connections_reset));
  std::printf("unflushed bytes lost %llu (the fault kill -9 cannot produce)\n",
              static_cast<unsigned long long>(result.bytes_lost_to_crashes));
  std::printf("network             %llu bytes delivered, %llu pongs matched\n",
              static_cast<unsigned long long>(result.bytes_delivered),
              static_cast<unsigned long long>(result.pongs));
}

int report_violation(const sim::SimulationResult& result) {
  std::printf("\n================ INVARIANT VIOLATED ================\n");
  std::printf("seed:       %llu (0x%llx)\n", static_cast<unsigned long long>(result.seed),
              static_cast<unsigned long long>(result.seed));
  std::printf("invariant:  %s\n", result.invariant != nullptr ? result.invariant : "?");
  std::printf("detail:     %s\n", result.detail.c_str());
  std::printf("at:         %.6f simulated seconds\n",
              base::to_seconds_f(result.simulated_time));
  std::printf("\nlast %zu trace events:\n", result.tail.size());
  for (const sim::TraceEvent& event : result.tail) {
    std::printf("  %s\n", sim::Trace::format(event).c_str());
  }
  std::printf("\nreplay it:  ./sim --seed %llu --dump-trace /tmp/trace.txt\n",
              static_cast<unsigned long long>(result.seed));
  std::printf("====================================================\n");
  return 1;
}

bool dump_trace(const sim::Trace& trace, const std::string& path) {
  std::FILE* file = std::fopen(path.c_str(), "w");
  if (file == nullptr) {
    std::fprintf(stderr, "sim: cannot write %s\n", path.c_str());
    return false;
  }
  for (const sim::TraceEvent& event : trace.captured()) {
    std::fprintf(file, "%s\n", sim::Trace::format(event).c_str());
  }
  std::fclose(file);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  sim::SimulationConfig config;
  base::u64 seeds = 10;
  bool single = false;
  bool quiet = false;
  std::string trace_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--seeds" && has_value) {
      seeds = std::strtoull(argv[++i], nullptr, 0);
    } else if (arg == "--seed" && has_value) {
      config.seed = std::strtoull(argv[++i], nullptr, 0);
      single = true;
    } else if (arg == "--nodes" && has_value) {
      config.node_count = static_cast<base::u32>(std::strtoul(argv[++i], nullptr, 0));
    } else if (arg == "--duration-s" && has_value) {
      config.duration = base::seconds(std::strtoll(argv[++i], nullptr, 0));
    } else if (arg == "--dump-trace" && has_value) {
      trace_path = argv[++i];
    } else if (arg == "--io-errors" && has_value) {
      config.faults.disk.io_error_probability = std::strtod(argv[++i], nullptr);
    } else if (arg == "--no-faults") {
      config.faults.crash_interval = 0;
      config.faults.partition_interval = 0;
      config.faults.clock_jump_interval = 0;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "-h" || arg == "--help") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "sim: unknown option %s\n", arg.c_str());
      usage();
      return 2;
    }
  }

  if (config.node_count == 0) {
    std::fprintf(stderr, "sim: --nodes must be at least 1\n");
    return 2;
  }

  if (single) {
    config.capture_trace = !trace_path.empty();
    sim::Simulation simulation(config);
    const sim::SimulationResult result = simulation.run();
    print_result(result, quiet);
    if (!trace_path.empty() && !dump_trace(simulation.trace(), trace_path)) return 2;
    return result.ok ? 0 : report_violation(result);
  }

  double node_hours = 0;
  base::u64 acked = 0;
  base::u64 crashes = 0;
  for (base::u64 seed = 1; seed <= seeds; ++seed) {
    config.seed = seed;
    const sim::SimulationResult result = run_simulation(config);
    node_hours += result.simulated_node_hours();
    acked += result.records_acked;
    crashes += result.crashes;
    if (!result.ok) {
      print_result(result, false);
      return report_violation(result);
    }
    if (!quiet && (seed % 10 == 0 || seed == seeds)) {
      std::printf("seed %llu/%llu ok — %.2f node-hours, %llu records acked so far\n",
                  static_cast<unsigned long long>(seed),
                  static_cast<unsigned long long>(seeds), node_hours,
                  static_cast<unsigned long long>(acked));
    } else if (quiet) {
      print_result(result, true);
    }
  }

  std::printf("\n%llu seeds green: %.2f simulated node-hours, %llu records acked, "
              "%llu crashes survived\n",
              static_cast<unsigned long long>(seeds), node_hours,
              static_cast<unsigned long long>(acked),
              static_cast<unsigned long long>(crashes));
  return 0;
}
