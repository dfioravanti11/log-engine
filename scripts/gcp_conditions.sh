#!/usr/bin/env bash
# gcp_conditions.sh — print the conditions a benchmark number is only meaningful with.
#
# §19: "Benchmark numbers ship with hardware, kernel, fs, mount options, offered load,
# and the exact command." This prints the first four. The benchmark itself prints the
# last two, so between them nothing about a published number is unstated.
#
# Run on the VM. `bench/run_gcp.sh` invokes it remotely so the header of a distributed
# run describes the machines the run actually happened on, not the laptop driving it.

set -uo pipefail

DATA="${LOGENGINE_DATA:-/var/lib/logengine}"
[ -d "$DATA" ] || DATA="$HOME"

DEV=$(df "$DATA" 2>/dev/null | awk 'NR==2{print $1}')

echo "host          $(uname -srm)"
echo "cpu           $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | xargs || echo unknown)"
echo "cores         $(nproc 2>/dev/null || echo unknown)"
echo "memory        $(awk '/MemTotal/{printf "%.1f GiB", $2/1048576}' /proc/meminfo 2>/dev/null || echo unknown)"
echo "data dir      $DATA"
echo "device        $DEV"
echo "filesystem    $(df -T "$DATA" 2>/dev/null | awk 'NR==2{print $2}' || echo unknown)"
echo "mount options $(findmnt -no OPTIONS --target "$DATA" 2>/dev/null || echo unknown)"
echo "scheduler     $(cat /sys/block/"$(lsblk -no PKNAME "$DEV" 2>/dev/null | head -1)"/queue/scheduler 2>/dev/null || echo unknown)"

# fsync on tmpfs returns immediately and durably stores nothing. A throughput number
# measured there is not an optimistic number, it is a fictional one — so this is an
# error rather than a warning.
if [ "$(df -T "$DATA" 2>/dev/null | awk 'NR==2{print $2}')" = "tmpfs" ]; then
  echo
  echo "REFUSING: $DATA is tmpfs. fsync() is a no-op there, so every durability and"
  echo "throughput number from this machine would be fiction. Point LOGENGINE_DATA at"
  echo "a real block device."
  exit 1
fi
