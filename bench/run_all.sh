#!/usr/bin/env bash
# run_all.sh — every number in the README, from one command.
#
# **Methodology before numbers, always** (§19). Each section below states its conditions
# with the number, because a p99 without an offered load is not a weak measurement, it is
# not a measurement.
#
#   BENCH_LOCAL=true ./bench/run_all.sh     # validate the harness on a laptop
#   ./bench/run_all.sh                      # a real 3-node cluster
#
# `BENCH_LOCAL=true` marks the run as harness validation. Those numbers never reach the
# README: three brokers sharing one laptop's CPU and one SSD are contending with each
# other for the exact resources being measured, so the throughput is meaningless and the
# tail is a measurement of the scheduler. The run still proves the harness works, which is
# what it is for.

set -uo pipefail
cd "$(dirname "$0")/.."

BENCH_LOCAL="${BENCH_LOCAL:-false}"
# `${RATES-...}` and not `${RATES:-...}`: the colon form treats empty as unset, which
# would make `RATES= ./bench/run_all.sh` silently run the default sweep instead of
# skipping it. Skipping is the whole point — see section 3.
RATES="${RATES-250 500 1000 2000 4000}"
RECORD_BYTES="${RECORD_BYTES:-1024}"
BATCH="${BATCH:-16}"
DURATION="${DURATION:-20}"
FAILURES="${FAILURES:-200}"
SEEDS="${SEEDS:-1000}"

BIN=build/dev/src/logengine
SIM=build/dev/tools/sim
FAILOVER=build/dev/bench/failover
WORK="${TMPDIR:-/tmp}/logengine_bench_$$"
BASE_PORT=9500

for exe in "$BIN" "$SIM" "$FAILOVER"; do
  [ -x "$exe" ] && continue
  echo "Build first:  cmake --preset dev && cmake --build --preset dev -j"
  echo "(missing: $exe)"
  exit 1
done

mkdir -p "$WORK"
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill -9 "$pid" 2>/dev/null; done
  rm -rf "$WORK"
}
trap cleanup EXIT

hr() { printf '=%.0s' {1..78}; echo; }

hr
echo "log-engine benchmark suite"
hr
echo "date          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "host          $(uname -srm)"
echo "cpu           $(sysctl -n machdep.cpu.brand_string 2>/dev/null || \
                      grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | xargs || echo unknown)"
echo "cores         $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unknown)"
echo "filesystem    $(df -T "$WORK" 2>/dev/null | awk 'NR==2{print $2}' || \
                      df "$WORK" | awk 'NR==2{print $1}')"
echo "mount options $(mount | grep -m1 "$(df "$WORK" | awk 'NR==2{print $1}')" | sed 's/.*(\(.*\))/\1/' || echo unknown)"
echo "build         $(cmake -LA -N build/dev 2>/dev/null | grep -m1 CMAKE_BUILD_TYPE | cut -d= -f2 || echo unknown)"
if [ "$BENCH_LOCAL" = "true" ]; then
  echo
  echo "*** BENCH_LOCAL=true — harness validation only. ***"
  echo "*** Three brokers on one machine contend for the CPU and disk being measured. ***"
  echo "*** These numbers validate the harness and never reach the README. ***"
fi
echo

hr
echo "1. simulator totals  (§19 #4)"
hr
"$SIM" --seeds "$SEEDS" --quiet | tail -n 2
echo "  bug journal entries: $(grep -c '^### #[0-9]' docs/retrospective.md) (docs/retrospective.md §1)"
echo

hr
echo "2. leader failover time  (§19 #3, NFR-3)"
hr
"$FAILOVER" --failures "$FAILURES"
FAILOVER_RC=$?
echo

hr
echo "3. sustained throughput and append-ack latency  (§19 #1, #2, NFR-1, NFR-2)"
hr
if [ -z "${RATES// /}" ]; then
  # `RATES= ./bench/run_all.sh` — the other three sections run on virtual time and are
  # hardware independent, so this is how you collect them alongside a real cluster sweep
  # from bench/run_gcp.sh without also running a meaningless loopback one.
  echo "skipped: RATES is empty."
  echo "The real-hardware version of this section is bench/run_gcp.sh, which runs one"
  echo "broker per VM. Sections 1, 2 and 4 above are unaffected — they run on virtual"
  echo "time, so this machine's hardware does not enter into them."
  echo
else
echo "A sweep, not a single number. NFR-2 asks for a p99 *at 70% of measured saturation*,"
echo "which means saturation has to be measured first — and the knee is the interesting"
echo "part anyway: below it the tail is flat, above it the queue is the latency."
echo
echo "records are ${RECORD_BYTES} B, ${BATCH} per batch, acks=quorum+fsync, 3 nodes"
echo "command       $BIN --bench-rate RATE --record-bytes $RECORD_BYTES --records $BATCH"
echo
printf "  %-10s %-12s %-10s %-10s %-10s %s\n" offered achieved MB/s p50 p99 terms
printf "  %-10s %-12s %-10s %-10s %-10s %s\n" ------- -------- ---- --- --- -----

peers_for() {
  local self=$1 out=""
  for n in 0 1 2; do
    [ "$n" = "$self" ] && continue
    out+="${n}@127.0.0.1:$((BASE_PORT + n)),"
  done
  echo "${out%,}"
}

run_at_rate() {
  local rate=$1 dir="$WORK/r$rate"
  rm -rf "$dir"; mkdir -p "$dir"
  local pids=()
  for n in 0 1 2; do
    "$BIN" --id "$n" --port $((BASE_PORT + n)) --dir "$dir/$n" \
           --peers "$(peers_for "$n")" --bench-rate "$rate" \
           --record-bytes "$RECORD_BYTES" --records "$BATCH" --status-ms 0 \
           --duration-s "$DURATION" > "$dir/$n.out" 2> "$dir/$n.err" &
    pids+=($!)
  done
  # Deliberately NOT disowned: this script needs `wait` to mean something. (An earlier
  # version copied the demo's `disown`, which suppresses the shell's job notices and also
  # makes `wait` return instantly — so every number was read out of an empty file.)
  wait "${pids[@]}" 2>/dev/null

  # Only the leader produced samples; followers correctly reject every proposal.
  local out=""
  for n in 0 1 2; do
    grep -q "leader=1" "$dir/$n.out" 2>/dev/null && out="$dir/$n.out"
  done
  [ -z "$out" ] && { printf "  %-10s no leader — cluster did not stabilise at this rate\n" "$rate"; return; }

  local achieved mb p50 p99 terms
  achieved=$(grep -o 'achieved  *[0-9]*' "$out" | awk '{print $2}')
  mb=$(grep 'achieved' "$out" | grep -o '[0-9.]* MB/s' | cut -d' ' -f1)
  p50=$(grep -o 'p50=[0-9. ]*ms' "$out" | head -1 | sed 's/p50=//;s/ *ms//')
  p99=$(grep -o 'p99=[0-9. ]*ms' "$out" | head -1 | sed 's/p99=//;s/ *ms//')
  terms=$(grep -ho 'term [0-9]*' "$dir"/*.err | awk '{print $2}' | sort -n | tail -1)
  printf "  %-10s %-12s %-10s %-10s %-10s %s\n" "$rate" "$achieved" "$mb" "$p50" "$p99" "$terms"
}

for rate in $RATES; do run_at_rate "$rate"; done
echo
echo "  'terms' is the highest Raft term any node reached. It should stay at 1: a term"
echo "  above that means the cluster was electing under load, which is a throughput"
echo "  result too — the event loop starving its own Raft ticks."
echo
fi

hr
echo "4. durability trade-off  (§19 #6, §13.2)"
hr
echo "Same seed, one knob. acks=quorum+fsync keeps every promise; acks=1 does not."
"$SIM" --seed 2 --duration-s 60 --crash-s 4 | grep -E "acked|faults"
if "$SIM" --seed 2 --duration-s 60 --crash-s 4 --acks-1 >/dev/null 2>&1; then
  echo "  BENCH FAILED: acks=1 was supposed to lose data on this seed."
  exit 1
fi
"$SIM" --seed 2 --duration-s 60 --crash-s 4 --acks-1 2>&1 | grep -E "^(invariant|detail):" | sed 's/^/  /'
echo

hr
echo "not measured yet"
hr
cat <<'NOTES'
  - Comparison against Kafka on identical hardware (§19 #7). The honest version needs
    both systems on the same VMs with the same offered load; anything less is a
    press release.
  - Consumer fetch throughput, zero-copy vs copy (§19 #8) — needs a client library.
  - End-to-end produce→consume latency (§19 #10) — the §12.1 gap between "on disk" and
    "readable". Needs a consumer.
  - One before/after optimization with a flamegraph (§19 #5). The target is named and
    the "before" is above: §13.1's group commit is specified and not implemented, so
    every append costs one synchronous fsync and the p50 you just read is one
    F_FULLFSYNC rather than anything this code does.
  - Throughput/latency on hardware that is not three brokers sharing one laptop. The
    numbers above validate the harness; by this project's own rule they do not go in
    the README.
NOTES
echo

hr
if [ "$BENCH_LOCAL" = "true" ]; then
  echo "harness validated. Re-run without BENCH_LOCAL on real hardware for README numbers."
else
  echo "benchmark suite complete."
fi
hr
exit $FAILOVER_RC
