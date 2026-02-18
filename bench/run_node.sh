#!/usr/bin/env bash
# run_node.sh — one node's half of a distributed sweep, for when you have a shell on each
# VM but no gcloud on your laptop (browser SSH, for instance).
#
# `bench/run_gcp.sh` does the same measurement by driving all three machines over
# `gcloud compute ssh`. This is the same sweep with the orchestration removed: paste it
# once into each of three terminals and the nodes keep themselves in lockstep.
#
# The lockstep is the only interesting part. Every node computes the same schedule from
# the same `--start` epoch — rate i begins at `start + i * (duration + gap)` — so nothing
# depends on the three pastes being simultaneous. They only have to happen before the
# first rate is due. GCE VMs track the metadata NTP server closely enough that a shared
# wall clock is the cheapest possible coordination.
#
#   ./bench/run_node.sh --id 0 --peers 1@10.x.x.x:9500,2@10.x.x.y:9500 --start 1755400000
#
# Only the leader produces samples, so exactly one of the three terminals prints a row
# per rate. Collect the rows; that is the table.

set -uo pipefail
cd "$(dirname "$0")/.."

ID=""; PEERS=""; START=""
while [ $# -gt 0 ]; do
  case "$1" in
    --id)    ID="${2:-}";    shift 2 ;;
    --peers) PEERS="${2:-}"; shift 2 ;;
    --start) START="${2:-}"; shift 2 ;;
    *) echo "unknown argument: $1"; exit 2 ;;
  esac
done

if [ -z "$ID" ] || [ -z "$PEERS" ] || [ -z "$START" ]; then
  echo "usage: $0 --id N --peers id@host:port,... --start EPOCH_SECONDS"
  echo
  echo "  pick EPOCH_SECONDS a couple of minutes out and give the SAME value to all three:"
  echo "      echo \$(( \$(date +%s) + 120 ))"
  exit 2
fi

RATES="${RATES-1000 2000 4000 8000 16000 32000}"
RECORD_BYTES="${RECORD_BYTES:-1024}"
BATCH="${BATCH:-16}"
DURATION="${DURATION:-30}"
GAP="${GAP:-12}"          # teardown + fresh election before the next rate
PORT="${PORT:-9500}"
DATA="${LOGENGINE_DATA:-/var/lib/logengine}"
BIN="$PWD/build/dev/src/logengine"

[ -x "$BIN" ] || { echo "missing $BIN — run scripts/gcp_setup.sh first"; exit 1; }

WORK="$HOME/logengine-bench-node$ID"
mkdir -p "$WORK"

count=0
for _ in $RATES; do count=$((count + 1)); done
total=$(( count * (DURATION + GAP) ))

echo "node $ID, peers $PEERS"
echo "rates: $RATES"
echo "${RECORD_BYTES} B records, ${BATCH} per batch, ${DURATION}s each, acks=quorum+fsync"
echo "starts at $START ($(( START - $(date +%s) ))s from now), runs ~$(( total / 60 ))m$(( total % 60 ))s"
echo "raw output under $WORK"
echo
printf "  %-10s %-12s %-10s %-10s %-10s %s\n" offered achieved MB/s p50 p99 terms
printf "  %-10s %-12s %-10s %-10s %-10s %s\n" ------- -------- ---- --- --- -----

i=0
for rate in $RATES; do
  at=$(( START + i * (DURATION + GAP) ))
  i=$(( i + 1 ))

  now=$(date +%s)
  if [ "$at" -lt "$now" ]; then
    echo "  (skipped $rate — its slot was $(( now - at ))s ago; start later next time)"
    continue
  fi
  while [ "$(date +%s)" -lt "$at" ]; do sleep 0.2; done

  # Scoped to this node's own invocation, not to the binary. Matching the binary path
  # would also kill the other two brokers whenever they happen to share a machine — which
  # is exactly how this script gets tested before it is trusted on three of them.
  pkill -f "logengine --id $ID --port $PORT" 2>/dev/null
  rm -rf "$DATA/r$rate"; mkdir -p "$DATA/r$rate"

  "$BIN" --id "$ID" --port "$PORT" --dir "$DATA/r$rate" --peers "$PEERS" --bind-all \
         --bench-rate "$rate" --record-bytes "$RECORD_BYTES" --records "$BATCH" \
         --status-ms 0 --duration-s "$DURATION" \
         > "$WORK/$rate.out" 2> "$WORK/$rate.err"

  # Followers correctly reject every proposal, so they have nothing to report.
  if ! grep -q "leader=1" "$WORK/$rate.out" 2>/dev/null; then
    printf "  %-10s (follower this round)\n" "$rate"
    continue
  fi

  achieved=$(grep -o 'achieved  *[0-9]*' "$WORK/$rate.out" | awk '{print $2}')
  mb=$(grep 'achieved' "$WORK/$rate.out" | grep -o '[0-9.]* MB/s' | cut -d' ' -f1)
  p50=$(grep -o 'p50=[0-9. ]*ms' "$WORK/$rate.out" | head -1 | sed 's/p50=//;s/ *ms//')
  p99=$(grep -o 'p99=[0-9. ]*ms' "$WORK/$rate.out" | head -1 | sed 's/p99=//;s/ *ms//')
  terms=$(grep -o 'term [0-9]*' "$WORK/$rate.err" | awk '{print $2}' | sort -n | tail -1)
  printf "  %-10s %-12s %-10s %-10s %-10s %s\n" "$rate" "$achieved" "$mb" "$p50" "$p99" "$terms"
done

echo
echo "  Rows above are the ones this node led. One node per rate has them; the other two"
echo "  say 'follower'. Together the three terminals are the table."
echo
echo "  'terms' should be 1. Higher means the cluster was electing under load — the event"
echo "  loop starving its own Raft ticks, which is a throughput result too."
echo
echo "  'achieved' covers the whole window including the startup election, so it"
echo "  understates steady state by about one election timeout in ${DURATION}s."
