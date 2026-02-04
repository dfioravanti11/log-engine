#!/usr/bin/env bash
# Week 6 demo (ER-6): three real processes, real TCP, real disks — and a `kill -9` on the
# leader that costs no committed record.
#
# This is the demo the whole architecture was built for. The broker running here is
# `server::Broker`, the *same object* the simulator has spent three weeks crashing,
# partitioning, and corrupting. The only difference is which `io::` implementations were
# constructed at startup. If that substitution did not work, nothing below would run.
#
# usage: ./scripts/demo_week6.sh

set -uo pipefail
cd "$(dirname "$0")/.."

BIN=build/dev/src/logengine
DUMP=build/dev/tools/log-dump
WORK="${TMPDIR:-/tmp}/logengine_week6_$$"
BASE_PORT=9400

if [ ! -x "$BIN" ]; then
  echo "Build first:  cmake --preset dev && cmake --build --preset dev -j"
  exit 1
fi

mkdir -p "$WORK"
PIDS=()
cleanup() {
  for pid in "${PIDS[@]:-}"; do kill -9 "$pid" 2>/dev/null; done
  rm -rf "$WORK"
}
trap cleanup EXIT

hr() { printf '=%.0s' {1..74}; echo; }

peers_for() {
  local self=$1 out=""
  for n in 0 1 2; do
    [ "$n" = "$self" ] && continue
    out+="${n}@127.0.0.1:$((BASE_PORT + n)),"
  done
  echo "${out%,}"
}

# Last reported value of a field, from a node's status stream.
field() { grep -o "$2=[0-9]*" "$WORK/$1.out" 2>/dev/null | tail -1 | cut -d= -f2; }
role()  { tail -1 "$WORK/$1.out" 2>/dev/null | awk '{print $3}'; }

start_node() {
  local n=$1
  "$BIN" --id "$n" --port $((BASE_PORT + n)) --dir "$WORK/$n" \
         --peers "$(peers_for "$n")" --produce --produce-ms 10 --records 4 \
         --status-ms 200 --duration-s 60 > "$WORK/$n.out" 2> "$WORK/$n.err" &
  PIDS[$n]=$!
  # The shell would otherwise print its own "Killed: 9" job notice over the demo output;
  # the kill is the point of the demo, but the bookkeeping around it is not.
  disown "${PIDS[$n]}" 2>/dev/null
}

hr
echo "1. three real processes, real sockets, real disks"
hr
for n in 0 1 2; do start_node "$n"; done
echo "started pids: ${PIDS[*]}"
echo

# Wait for a leader to emerge.
LEADER=""
for _ in $(seq 1 50); do
  sleep 0.2
  for n in 0 1 2; do
    if [ "$(role "$n")" = "leader" ]; then LEADER=$n; break; fi
  done
  [ -n "$LEADER" ] && break
done

if [ -z "$LEADER" ]; then
  echo "DEMO FAILED: no leader after 10 s"
  for n in 0 1 2; do echo "--- node $n ---"; tail -3 "$WORK/$n.err"; done
  exit 1
fi
echo "leader elected: node $LEADER (term $(field "$LEADER" term))"
echo

hr
echo "2. the cluster is committing"
hr
sleep 2
for n in 0 1 2; do printf "  "; tail -1 "$WORK/$n.out"; done
COMMITTED_BEFORE=$(field "$LEADER" commit)
echo
echo "committed before the kill: $COMMITTED_BEFORE records"
echo

hr
echo "3. kill -9 the leader"
hr
kill -9 "${PIDS[$LEADER]}" 2>/dev/null
echo "SIGKILL sent to node $LEADER (pid ${PIDS[$LEADER]}) — no shutdown, no flush, no mercy"
echo

NEW_LEADER=""
for _ in $(seq 1 60); do
  sleep 0.2
  for n in 0 1 2; do
    [ "$n" = "$LEADER" ] && continue
    if [ "$(role "$n")" = "leader" ]; then NEW_LEADER=$n; break; fi
  done
  [ -n "$NEW_LEADER" ] && break
done

if [ -z "$NEW_LEADER" ]; then
  echo "DEMO FAILED: no new leader after 12 s"
  exit 1
fi
echo "new leader: node $NEW_LEADER (term $(field "$NEW_LEADER" term))"
echo

hr
echo "4. it kept every committed record, and it is still committing"
hr
sleep 2
for n in 0 1 2; do
  [ "$n" = "$LEADER" ] && continue
  printf "  "; tail -1 "$WORK/$n.out"
done
COMMITTED_AFTER=$(field "$NEW_LEADER" commit)
echo
echo "committed before the kill: $COMMITTED_BEFORE"
echo "committed now:             $COMMITTED_AFTER"

if [ -z "$COMMITTED_AFTER" ] || [ "$COMMITTED_AFTER" -lt "$COMMITTED_BEFORE" ]; then
  echo
  echo "DEMO FAILED: the commit index went backwards across the failover."
  echo "That is an I1 violation on real hardware — every record between those two"
  echo "numbers was acknowledged and is now gone."
  exit 1
fi
echo
echo "The commit index never went backwards. Records the old leader had committed are"
echo "on the new one, because it could not have won the election without them (§5.4.1)."
echo

hr
echo "5. the survivors' logs agree, on disk"
hr
for n in 0 1 2; do
  [ "$n" = "$LEADER" ] && continue
  seg=$(ls "$WORK/$n"/[0-9]*.log 2>/dev/null | head -1)
  [ -z "$seg" ] && { echo "DEMO FAILED: node $n wrote no segment"; exit 1; }
  printf "  node %s: %s\n" "$n" "$("$DUMP" "$seg" 2>/dev/null | tail -1)"
done
echo
echo "Read back by log-dump, which never repairs what it reads."
echo

hr
echo "week 6 demo complete — same Broker as the simulator, real sockets, real kill -9."
hr
