#!/usr/bin/env bash
# run_gcp.sh — §19 #1 and #2 on three machines instead of one.
#
# **Why this script exists at all.** `bench/run_all.sh` runs its throughput sweep as three
# brokers on 127.0.0.1, and says in its own header that those numbers never reach the
# README. Two things are wrong with them and only one is the obvious one:
#
#   - the three brokers contend for one CPU and one disk, which is the objection everyone
#     makes, and
#   - replication happens over loopback, which is the objection that actually matters. A
#     quorum commit whose round trip costs ~20 µs is not a distributed system's number.
#     It flatters exactly the thing this project is about.
#
# So the sweep moves to three VMs in one zone and the other three sections of run_all.sh
# stay where they are — sections 1, 2 and 4 run on virtual time, so they are hardware
# independent by construction and a GCP run would produce identical output.
#
#   ZONE=europe-west4-a ./bench/run_gcp.sh
#
# Reads: ZONE (required), PREFIX, RATES, RECORD_BYTES, BATCH, DURATION, PORT, SSH_FLAGS.

set -uo pipefail
cd "$(dirname "$0")/.."

ZONE="${ZONE:?set ZONE, e.g. ZONE=europe-west4-a}"
PREFIX="${PREFIX:-logengine-bench}"
RATES="${RATES:-1000 2000 4000 8000 16000 32000}"
RECORD_BYTES="${RECORD_BYTES:-1024}"
BATCH="${BATCH:-16}"
DURATION="${DURATION:-30}"
PORT="${PORT:-9500}"
SSH_FLAGS="${SSH_FLAGS:-}"
SRC="${LOGENGINE_SRC:-/home/$(whoami)/log-engine}"
DATA="${LOGENGINE_DATA:-/var/lib/logengine}"

WORK="${TMPDIR:-/tmp}/logengine_gcp_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

command -v gcloud >/dev/null || { echo "gcloud not found"; exit 1; }

hr() { printf '=%.0s' {1..78}; echo; }

# ---------------------------------------------------------------- discover the cluster
NODES=(); IPS=()
for n in 0 1 2; do
  NODES+=("$PREFIX-$n")
  ip=$(gcloud compute instances describe "$PREFIX-$n" --zone "$ZONE" \
         --format='get(networkInterfaces[0].networkIP)' 2>/dev/null)
  if [ -z "$ip" ]; then
    echo "no instance $PREFIX-$n in $ZONE — create it first (see docs/benchmarking.md)"
    exit 1
  fi
  IPS+=("$ip")
done

ssh_node() {  # ssh_node <index> <command>
  gcloud compute ssh "${NODES[$1]}" --zone "$ZONE" --quiet $SSH_FLAGS --command "$2"
}

peers_for() {
  local self=$1 out=""
  for n in 0 1 2; do
    [ "$n" = "$self" ] && continue
    out+="${n}@${IPS[$n]}:${PORT},"
  done
  echo "${out%,}"
}

hr
echo "log-engine throughput sweep — 3 VMs, one broker each"
hr
echo "date          $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "zone          $ZONE"
echo "machine type  $(gcloud compute instances describe "$PREFIX-0" --zone "$ZONE" \
                        --format='value(machineType.basename())' 2>/dev/null)"
BOOT=$(gcloud compute instances describe "$PREFIX-0" --zone "$ZONE" \
         --format='value(disks[0].source.basename())' 2>/dev/null)
echo "disk          $(gcloud compute disks describe "$BOOT" --zone "$ZONE" \
                        --format='value(type.basename(), sizeGb)' 2>/dev/null | tr '\t' ' ')"
echo "nodes         ${NODES[*]}  (${IPS[*]})"
echo
ssh_node 0 "$SRC/scripts/gcp_conditions.sh" 2>/dev/null || {
  echo "could not read conditions from ${NODES[0]} — is scripts/gcp_setup.sh done there?"
  exit 1
}
echo "commit        $(ssh_node 0 "git -C $SRC log --oneline -1" 2>/dev/null)"
echo

hr
echo "sustained throughput and append-ack latency  (§19 #1, #2, NFR-1, NFR-2)"
hr
echo "A sweep, not a single number: NFR-2 wants a p99 at 70% of measured saturation, so"
echo "saturation has to be measured before it can be divided — and the knee is the"
echo "interesting part anyway."
echo
echo "records are ${RECORD_BYTES} B, ${BATCH} per batch, acks=quorum+fsync, 3 nodes, ${DURATION}s each"
echo "command       logengine --bind-all --bench-rate RATE --record-bytes $RECORD_BYTES --records $BATCH"
echo
printf "  %-10s %-12s %-10s %-10s %-10s %s\n" offered achieved MB/s p50 p99 terms
printf "  %-10s %-12s %-10s %-10s %-10s %s\n" ------- -------- ---- --- --- -----

run_at_rate() {
  local rate=$1 dir="$WORK/r$rate"
  mkdir -p "$dir"

  # All three nodes wait for the same wall-clock second before exec'ing the broker.
  # Without it, `gcloud ssh` start skew (seconds, and variable) lands inside the
  # measurement window: the leader's clock starts when *it* starts, so a late third node
  # shows up as reduced throughput that has nothing to do with the code. GCE VMs track
  # the metadata NTP server closely enough that a shared epoch is worth more than the
  # skew it removes.
  local start=$(( $(date +%s) + 12 ))

  local pids=()
  for n in 0 1 2; do
    ssh_node "$n" "pkill -f 'logengine --id $n --port $PORT' 2>/dev/null; \
      rm -rf '$DATA/r$rate' && mkdir -p '$DATA/r$rate/$n' && \
      while [ \"\$(date +%s)\" -lt $start ]; do sleep 0.05; done; \
      exec '$SRC/build/dev/src/logengine' --id $n --port $PORT \
        --dir '$DATA/r$rate/$n' --peers '$(peers_for "$n")' --bind-all \
        --bench-rate $rate --record-bytes $RECORD_BYTES --records $BATCH \
        --status-ms 0 --duration-s $DURATION" \
      > "$dir/$n.out" 2> "$dir/$n.err" &
    pids+=($!)
  done
  wait "${pids[@]}" 2>/dev/null

  # Only the leader produced samples; followers correctly reject every proposal.
  local out=""
  for n in 0 1 2; do
    grep -q "leader=1" "$dir/$n.out" 2>/dev/null && out="$dir/$n.out"
  done
  if [ -z "$out" ]; then
    printf "  %-10s no leader — cluster did not stabilise (see %s)\n" "$rate" "$dir"
    cp -R "$dir" "./bench-failed-r$rate" 2>/dev/null
    return
  fi

  local achieved mb p50 p99 terms
  achieved=$(grep -o 'achieved  *[0-9]*' "$out" | awk '{print $2}')
  mb=$(grep 'achieved' "$out" | grep -o '[0-9.]* MB/s' | cut -d' ' -f1)
  p50=$(grep -o 'p50=[0-9. ]*ms' "$out" | head -1 | sed 's/p50=//;s/ *ms//')
  p99=$(grep -o 'p99=[0-9. ]*ms' "$out" | head -1 | sed 's/p99=//;s/ *ms//')
  terms=$(grep -ho 'term [0-9]*' "$dir"/*.err | awk '{print $2}' | sort -n | tail -1)
  printf "  %-10s %-12s %-10s %-10s %-10s %s\n" "$rate" "$achieved" "$mb" "$p50" "$p99" "$terms"
  echo "$rate $achieved $mb $p50 $p99 $terms" >> "$WORK/table"
}

for rate in $RATES; do run_at_rate "$rate"; done

echo
echo "  'terms' is the highest Raft term any node reached. It should stay at 1: above that"
echo "  means the cluster was electing under load, which is a throughput result too — the"
echo "  event loop starving its own Raft ticks."
echo
echo "  'achieved' counts committed records over the whole window, election included, so"
echo "  it understates steady state by roughly one election timeout out of ${DURATION}s."
echo
echo "  Latency is open-loop: measured from when a record was *due* to be issued, so a"
echo "  stall stays in the histogram instead of vanishing exactly when it matters."
echo

hr
echo "still not measured on this hardware"
hr
cat <<'NOTES'
  - §13.1 group commit is specified and NOT implemented, so every append costs one
    synchronous fsync and the p50 above is one device flush plus one network round trip.
    That is the "before" number for §19 #5; the "after" does not exist yet.
  - Kafka on identical hardware (§19 #7). Same VMs, same offered load, or it is a press
    release rather than a comparison.
  - Consumer fetch and end-to-end produce→consume latency (§19 #8, #10) — both need the
    client library, which is not built.
NOTES
echo
hr
echo "sections 1, 2 and 4 (simulator totals, failover, durability trade-off) run on"
echo "virtual time and are hardware independent. Get them from any machine:"
echo "    RATES= ./bench/run_all.sh"
hr
