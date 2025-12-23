#!/usr/bin/env bash
# ER-1, "the one rule": nothing in storage/, raft/, server/, or client/ may touch the
# OS directly. No <chrono> clocks, no sockets, no file I/O, no rand(), no threads.
# Everything goes through an io/ interface injected at construction.
#
# This is what makes the simulator possible, and it is trivially easy to violate at
# 2am. Ten lines of shell that saves the project (§11).

set -uo pipefail
cd "$(dirname "$0")/.."

GUARDED_DIRS=(src/storage src/raft src/server src/client)

# --self-test plants a known violation, confirms the guard catches it, and removes
# it. Until week 2 the guarded directories do not exist, so a plain run passes
# vacuously — and a check that cannot fail is not a check. CI runs this first.
if [ "${1:-}" = "--self-test" ]; then
  probe_dir="src/storage"
  probe="$probe_dir/__er1_selftest.cpp"
  created_dir=0
  [ -d "$probe_dir" ] || { mkdir -p "$probe_dir"; created_dir=1; }
  printf '#include <chrono>\nauto t = std::chrono::steady_clock::now();\n' > "$probe"

  if "$0" >/dev/null 2>&1; then
    rm -f "$probe"; [ "$created_dir" = 1 ] && rmdir "$probe_dir" 2>/dev/null
    echo "SELF-TEST FAILED: the guard did not catch a planted <chrono> violation."
    exit 1
  fi

  rm -f "$probe"; [ "$created_dir" = 1 ] && rmdir "$probe_dir" 2>/dev/null
  echo "ER-1 self-test OK: planted violation was caught"
  exit 0
fi

# pattern|human-readable reason
PATTERNS=(
  '#include[[:space:]]*<sys/|direct syscall header'
  '#include[[:space:]]*<netinet/|direct socket header'
  '#include[[:space:]]*<arpa/|direct socket header'
  '#include[[:space:]]*<unistd\.h>|direct syscall header'
  '#include[[:space:]]*<fcntl\.h>|direct file I/O header'
  '#include[[:space:]]*<chrono>|use io::Clock, not <chrono>'
  '#include[[:space:]]*<thread>|use the event loop, not std::thread'
  '#include[[:space:]]*<random>|use io::Random, not <random>'
  '#include[[:space:]]*<cstdio>|use io::Disk, not stdio'
  '#include[[:space:]]*<fstream>|use io::Disk, not fstream'
  '#include[[:space:]]*<filesystem>|use io::Disk, not std::filesystem'
  'chrono::[A-Za-z_]*::now|use io::Clock::monotonic_now()'
  '[^_a-zA-Z]rand\(|use io::Random'
  'std::this_thread|use the event loop'
  'std::thread|use the event loop'
  'std::mutex|partition state is single-threaded by design'
)

violations=0
for dir in "${GUARDED_DIRS[@]}"; do
  [ -d "$dir" ] || continue
  for entry in "${PATTERNS[@]}"; do
    pattern="${entry%%|*}"
    reason="${entry#*|}"
    while IFS= read -r hit; do
      [ -z "$hit" ] && continue
      echo "ER-1 violation: $hit"
      echo "                ^ $reason"
      violations=$((violations + 1))
    done < <(grep -rInE "$pattern" "$dir" --include='*.h' --include='*.cpp' 2>/dev/null)
  done
done

if [ "$violations" -gt 0 ]; then
  echo
  echo "FAILED: $violations violation(s) of the one rule (ER-1)."
  echo "If a change seems to need an exception, the design is wrong, not the rule."
  exit 1
fi

echo "ER-1 OK: no direct OS access in ${GUARDED_DIRS[*]}"
