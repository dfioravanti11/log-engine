#!/usr/bin/env bash
# Week 2 demo (ER-6): kill -9 mid-append, restart, prove no acked record was lost.
#
# Two phases, because they prove different things and only one of them is a real crash.
#
#   Phase 1 — kill -9 rounds. The appender fsyncs before it acks; SIGKILL gives it no
#   chance to flush, close, or clean up. Every ack ever recorded, across every round,
#   must still be readable after the restart.
#
#   Phase 2 — an injected torn tail. Worth being blunt about: **kill -9 cannot tear a
#   write.** Killing a process does not drop the page cache, so everything it handed to
#   pwrite is still there for the next process to read. A genuinely half-written batch
#   needs power loss, a disk that lies about fsync, or a kill landing inside a pwrite
#   large enough to be interrupted. So phase 2 appends garbage bytes to the newest
#   segment by hand — exactly what a partial batch looks like on disk — and checks that
#   recovery drops it and the acks still hold.
#
# Phase 2 is a stand-in. Week 3's simulator injects unflushed-write loss for real, on
# virtual time, from a seed.
#
# usage: ./scripts/demo_week2.sh [rounds]

set -uo pipefail
cd "$(dirname "$0")/.."

ROUNDS="${1:-5}"
BUILD_DIR="build/dev"
DATA_DIR="${TMPDIR:-/tmp}/logengine_demo_$$"
ACKS="$DATA_DIR/acks.txt"
APPEND_OUT="$DATA_DIR/append.out"

CRASH_DEMO="$BUILD_DIR/tools/crash-demo"
LOG_DUMP="$BUILD_DIR/tools/log-dump"

if [ ! -x "$CRASH_DEMO" ] || [ ! -x "$LOG_DUMP" ]; then
  echo "Build first:  cmake --preset dev && cmake --build --preset dev -j"
  exit 1
fi

mkdir -p "$DATA_DIR"
trap 'rm -rf "$DATA_DIR"' EXIT

# Segment files are 20 digits + .log, so this glob can never pick up acks.txt or
# append.out. (It did, the first time this script ran.)
newest_segment() {
  ls "$DATA_DIR"/[0-9]*.log 2>/dev/null | sort | tail -n 1
}

fail() {
  echo
  echo "DEMO FAILED: $1"
  echo "Data dir kept for inspection: $DATA_DIR"
  trap - EXIT
  exit 1
}

echo "data dir: $DATA_DIR"
echo

for round in $(seq 1 "$ROUNDS"); do
  echo "=============================================================="
  echo "round $round/$ROUNDS — append until killed"
  echo "=============================================================="

  "$CRASH_DEMO" append --dir "$DATA_DIR" --acks "$ACKS" --records-per-batch 4 \
    >/dev/null 2>"$APPEND_OUT" &
  pid=$!

  sleep "0.$((RANDOM % 5 + 3))"
  kill -9 "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  echo "SIGKILLed pid $pid mid-append"
  tail -n 1 "$APPEND_OUT"
  echo

  echo "--- restart: reopen the log and check every ack ---"
  "$CRASH_DEMO" verify --dir "$DATA_DIR" --acks "$ACKS" || fail "round $round lost an acked record"
  echo
done

echo "=============================================================="
echo "phase 2 — inject a torn tail (kill -9 alone cannot produce one)"
echo "=============================================================="

segment=$(newest_segment)
[ -n "$segment" ] || fail "no segment file found"

before=$(wc -c <"$segment" | tr -d ' ')
# 37 bytes: shorter than a batch header, so it is a batch that started and never
# finished — the shape a partial pwrite leaves behind.
head -c 37 /dev/urandom >>"$segment"
echo "appended 37 bytes of garbage to $(basename "$segment") ($before -> $(wc -c <"$segment" | tr -d ' ') bytes)"
echo

echo "--- log-dump BEFORE recovery: the tool is read-only, so the damage is visible ---"
"$LOG_DUMP" "$segment" --quiet
echo

echo "--- restart: recovery truncates the tail, acks must still hold ---"
"$CRASH_DEMO" verify --dir "$DATA_DIR" --acks "$ACKS" || fail "torn tail cost an acked record"
echo

echo "--- log-dump AFTER recovery: the tail is gone, the log ends on a batch boundary ---"
"$LOG_DUMP" "$segment" --quiet
echo

echo "=============================================================="
echo "$ROUNDS kill -9 rounds + 1 torn tail, every acked record present."
echo "=============================================================="
