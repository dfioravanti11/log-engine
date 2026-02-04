#!/usr/bin/env bash
# Week 4 demo (ER-6): leader election, under faults, with I6 checked every event.
#
# Five parts:
#   1. A healthy cluster elects one leader and *keeps* it. One election, one term.
#   2. 1000 seeds green under crashes, partitions and clock jumps, with I6 checked.
#   3. The headline (§13.2's shape, applied to Raft metadata): one seed, one knob.
#      With the raft.state fsync on, clean. With it off, one term elects two leaders.
#   4. Bug journal #2 — one cut link used to make leadership ping-pong every 200 ms.
#   5. What is *not* fixed yet, with the number attached.
#
# usage: ./scripts/demo_week4.sh [seeds]

set -uo pipefail
cd "$(dirname "$0")/.."

SEEDS="${1:-1000}"
SIM="build/dev/tools/sim"

if [ ! -x "$SIM" ]; then
  echo "Build first:  cmake --preset dev && cmake --build --preset dev -j"
  exit 1
fi

hr() { printf '=%.0s' {1..74}; echo; }

hr
echo "1. a healthy cluster elects one leader and keeps it"
hr
echo "Two simulated minutes, no faults. One election, one term — if heartbeats were not"
echo "holding off the followers' timers this would climb, and cost a failover each time."
echo
"$SIM" --seed 1 --duration-s 120 --no-faults | grep -E "simulated|raft"
echo

hr
echo "2. $SEEDS seeds under crashes, partitions and clock jumps — I6 checked every event"
hr
"$SIM" --seeds "$SEEDS" --quiet | tail -n 2 || exit 1
echo

hr
echo "3. the headline: one seed, one knob — the fsync of currentTerm/votedFor"
hr
echo "§13 says this is the one durability knob that is never tunable. Here is why,"
echo "on a single seed. First with it ON, as it ships:"
echo
"$SIM" --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120 | grep -E "faults|raft"
echo
echo "Now the identical seed with the fsync skipped — the write still happens, it just"
echo "is not waited on, so a power cut can take the vote with it:"
echo
if "$SIM" --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120 --unsafe-metadata \
     >/dev/null 2>&1; then
  echo "DEMO FAILED: the unsafe run was supposed to violate I6 and did not."
  echo "Either the checker stopped checking or the fault stopped being injected."
  exit 1
fi
"$SIM" --seed 4 --duration-s 120 --crash-s 3 --restart-ms 120 --unsafe-metadata 2>&1 \
  | grep -E "^(seed|invariant|detail|at):"
echo
echo "A node voted, crashed before the write reached the platter, came back not"
echo "remembering, and voted again in the same term. Two candidates, two majorities,"
echo "one term. No bug anywhere in the algorithm — just a missing fsync."
echo

hr
echo "4. bug journal #2 — the livelock the invariant set could not see"
hr
echo "One cut link, held for 30 s. Before the §4.2.3 lease rule, leadership ping-ponged"
echo "every ~200 ms for the whole partition: 28 elections in 40 simulated seconds, while"
echo "every one of I1-I6 held. Safe, and completely unavailable. After:"
echo
"$SIM" --seed 3 --duration-s 40 --crash-s 0 --partition-s 30 | grep -E "faults|raft"
echo
echo "One election. The node behind the cut still burns terms campaigning into the void —"
echo "that is term inflation, and Pre-Vote is the fix. Deferred to week 5, on purpose:"
echo "the livelock was the availability bug, the inflation costs one election on heal."
echo

hr
echo "5. determinism still holds with Raft in the loop (I7)"
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
echo "week 4 demo complete."
hr
