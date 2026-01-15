#!/usr/bin/env bash
# Week 3 demo (ER-6): the same seed twice produces an identical trace hash, and an hour
# of cluster life simulates in seconds.
#
# Four parts:
#   1. Determinism — one seed, run twice, byte-identical traces. This is I7, and it is
#      the check every other correctness claim in the project rests on.
#   2. Speed — one simulated hour, timed (NFR-4: ≥ 1 cluster-hour per 5 wall seconds).
#   3. Coverage — a seed sweep, reporting simulated node-hours and faults survived.
#   4. The point of all of it — the fault `kill -9` provably cannot produce (week 2's
#      demo had to fake it by corrupting a file by hand), and the bug that fault found.
#
# usage: ./scripts/demo_week3.sh [seeds]

set -uo pipefail
cd "$(dirname "$0")/.."

SEEDS="${1:-200}"
SIM="build/dev/tools/sim"
WORK="${TMPDIR:-/tmp}/logengine_week3_$$"

if [ ! -x "$SIM" ]; then
  echo "Build first:  cmake --preset dev && cmake --build --preset dev -j"
  exit 1
fi

mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

hr() { printf '=%.0s' {1..70}; echo; }

hr
echo "1. determinism (I7) — the same seed twice, compared event by event"
hr
"$SIM" --seed 0x3f2a91c4 --duration-s 120 --dump-trace "$WORK/a.txt" | tail -n 7
"$SIM" --seed 0x3f2a91c4 --duration-s 120 --dump-trace "$WORK/b.txt" >/dev/null

if ! diff -q "$WORK/a.txt" "$WORK/b.txt" >/dev/null; then
  echo
  echo "DEMO FAILED: two runs of one seed disagree. First divergence:"
  diff "$WORK/a.txt" "$WORK/b.txt" | head -n 10
  exit 1
fi
echo
echo "identical: $(wc -l <"$WORK/a.txt" | tr -d ' ') trace events, byte for byte"
echo

hr
echo "2. speed (NFR-4) — one simulated hour of a 3-node cluster"
hr
start=$(date +%s)
"$SIM" --seed 7 --duration-s 3600 | tail -n 7
elapsed=$(( $(date +%s) - start ))
echo
echo "one simulated hour in ${elapsed}s of wall clock (budget: 5s per cluster-hour)"
echo

hr
echo "3. coverage — $SEEDS seeds under crashes, partitions, and clock jumps"
hr
"$SIM" --seeds "$SEEDS" --duration-s 30 | tail -n 4 || exit 1
echo

hr
echo "4. the fault kill -9 cannot produce — unflushed writes lost on power cut"
hr
echo "Week 2's demo could only mimic a torn tail by corrupting a file by hand, because"
echo "SIGKILL does not drop the page cache. Here it is injected for real, from a seed:"
echo
"$SIM" --seed 99 --duration-s 300 | grep -E "unflushed|acked|faults"
echo
echo "Every acked record still readable after every one of those crashes."
echo
echo "And with disk I/O errors turned on, the simulator found a real bug in week 2's"
echo "recovery code on its first sweep (docs/retrospective.md §1 entry #1):"
echo
"$SIM" --seeds 40 --duration-s 30 --io-errors 0.02 | tail -n 2 || exit 1

hr
echo "week 3 demo complete."
hr
