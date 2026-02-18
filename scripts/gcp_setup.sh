#!/usr/bin/env bash
# gcp_setup.sh — turn a bare GCE VM into a benchmark node. Idempotent; re-run freely.
#
# Run ON each VM (bench/run_gcp.sh can push it there for you):
#
#   ./scripts/gcp_setup.sh [git-url] [ref]
#
# Installs the toolchain, builds the three binaries the benchmark needs, creates the
# data directory, and refuses to finish if that directory is tmpfs.

set -euo pipefail

REPO="${1:-https://github.com/dfioravanti11/log-engine.git}"
REF="${2:-main}"
SRC="${LOGENGINE_SRC:-$HOME/log-engine}"
DATA="${LOGENGINE_DATA:-/var/lib/logengine}"

echo "==> toolchain"
sudo apt-get update -qq
# GCC 13 is CI's second frontend (see .github/workflows/ci.yml) and is the default on
# Ubuntu 24.04, so this is a compiler the code is already known to build under.
sudo apt-get install -y -qq --no-install-recommends \
  git cmake ninja-build build-essential ca-certificates

echo "==> source"
if [ -d "$SRC/.git" ]; then
  git -C "$SRC" fetch --all --prune --quiet
  git -C "$SRC" checkout --quiet "$REF"
  git -C "$SRC" pull --ff-only --quiet || true
  echo "    $(git -C "$SRC" log --oneline -1)"
elif [ -f "$SRC/CMakeLists.txt" ]; then
  # A tree that is already here and is not a checkout: the `git archive` route, which is
  # how a private repo gets onto a VM that has no credentials and should not be given
  # any. Nothing to fetch — build what was shipped.
  echo "    using the tree already at $SRC (no checkout)"
else
  git clone --quiet "$REPO" "$SRC" || {
    echo
    echo "clone failed. If the repository is private, do not put a credential on this VM —"
    echo "ship the source instead:  git archive --format=tar.gz --prefix=log-engine/ \\"
    echo "                            -o logengine-src.tgz HEAD"
    echo "upload it, 'tar xzf logengine-src.tgz -C ~', then re-run this script."
    exit 1
  }
  git -C "$SRC" checkout --quiet "$REF"
  echo "    $(git -C "$SRC" log --oneline -1)"
fi

echo "==> build"
cd "$SRC"
cmake --preset dev >/dev/null
# Only what the benchmark runs. Skipping the test binaries keeps a cold VM under a
# minute; correctness is CI's job and was settled before anything got here.
cmake --build --preset dev -j"$(nproc)" \
  --target logengine tool_sim bench_failover 2>&1 | tail -3

echo "==> data directory"
sudo mkdir -p "$DATA"
sudo chown "$(id -un):$(id -gn)" "$DATA"

echo
"$SRC/scripts/gcp_conditions.sh"
echo
echo "ready: $SRC/build/dev/src/logengine"
