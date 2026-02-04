#!/usr/bin/env bash
# Week 5 demo (ER-6): a replicated log, and the durability trade-off made visible.
#
# Four parts:
#   1. Replication works — one leader, one term, every record committed by a majority.
#   2. The headline (§13.2): one seed, one knob. `acks=quorum+fsync` keeps every promise
#      through thirty crashes; `acks=1` loses records the moment a leader dies early.
#   3. Liveness (I8) — the invariant journal #2 said was missing, and the number no
#      safety check can see.
#   4. Determinism still holds (I7) with replication in the loop.
#
# usage: ./scripts/demo_week5.sh [seeds]

set -uo pipefail
cd "$(dirname "$0")/.."

SEEDS="${1:-500}"
SIM="build/dev/tools/sim"

if [ ! -x "$SIM" ]; then
  echo "Build first:  cmake --preset dev && cmake --build --preset dev -j"
  exit 1
fi

hr() { printf '=%.0s' {1..74}; echo; }

hr
echo "1. a replicated log — one leader, one term, majority-committed"
hr
echo "One simulated minute, no faults. Every record below was written by the leader,"
echo "replicated to a majority, and fsynced there before the producer was told yes."
echo
"$SIM" --seed 7 --duration-s 60 --no-faults | grep -E "acked|raft|longest"
echo

hr
echo "2. $SEEDS seeds under crashes, partitions and clock jumps"
hr
"$SIM" --seeds "$SEEDS" --quiet | tail -n 2 || exit 1
echo

hr
echo "3. the headline (§13.2): acks=1 vs acks=quorum+fsync, same seed"
hr
echo "Seed 2, thirty crashes. First as it ships — acks=quorum+fsync, where a record is"
echo "acked only once a majority holds it durably:"
echo
"$SIM" --seed 2 --duration-s 60 --crash-s 4 | grep -E "acked|faults"
echo
echo "Now the identical seed with acks=1: the leader answers the producer the moment its"
echo "own fsync returns, without waiting for a single peer."
echo
if "$SIM" --seed 2 --duration-s 60 --crash-s 4 --acks-1 >/dev/null 2>&1; then
  echo "DEMO FAILED: acks=1 was supposed to lose data on this seed and did not."
  echo "Either the fault stopped being injected or the promise stopped being checked."
  exit 1
fi
"$SIM" --seed 2 --duration-s 60 --crash-s 4 --acks-1 2>&1 \
  | grep -E "^(seed|invariant|detail|at):"
echo
echo "Records the producer was told were safe, on a leader that died before replicating"
echo "them. A new leader was elected that had never seen them — correctly, by every rule"
echo "in the paper. Neither setting is a bug. They are different promises, and the"
echo "simulator holds the system to whichever one it made."
echo

hr
echo "4. liveness (I8) — the number no safety invariant can see"
hr
echo "I1-I6 are all safety properties, and a cluster that does nothing satisfies every"
echo "one of them. That is how bug journal #2 hid behind a thousand green seeds. So the"
echo "simulator now measures the longest stretch with no leader anywhere:"
echo
printf "  healthy:      "; "$SIM" --seed 7 --duration-s 120 --no-faults | grep longest
printf "  under faults: "; "$SIM" --seed 7 --duration-s 120 --crash-s 8 | grep longest
echo
echo "Stated conditionally on purpose. During a partition that costs the majority, having"
echo "no leader is *correct* — so the invariant is not 'a leader exists', it is 'the"
echo "cluster gets one back within a bounded time of being able to'."
echo

hr
echo "5. determinism still holds with replication in the loop (I7)"
hr
a=$("$SIM" --seed 0x3f2a91c4 --duration-s 120 | grep "trace hash")
b=$("$SIM" --seed 0x3f2a91c4 --duration-s 120 | grep "trace hash")
if [ "$a" != "$b" ]; then
  echo "DEMO FAILED: two runs of one seed disagree."
  echo "  $a"
  echo "  $b"
  exit 1
fi
echo "$a"
echo "identical across runs."
echo

hr
echo "week 5 demo complete."
hr
