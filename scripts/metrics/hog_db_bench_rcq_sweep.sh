#!/usr/bin/env bash
set -e

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1
source "$(dirname "${BASH_SOURCE[0]}")/hog_db_bench_common.sh"

# Entries per RCQ internal block (must match kRetrocausalQueueBlockEntriesMaxLen).
ENTRIES_PER_BLOCK=1000
# Number of blockchain blocks per run.  Keeping this constant across queue
# counts means throughput reflects queue parallelism rather than commit overhead.
BLOCKS=10
WORKERS=24
QUEUE_COUNTS="4 16 64 256 1024 4096"

for q in $QUEUE_COUNTS; do
  n=$((BLOCKS * q * ENTRIES_PER_BLOCK))

  run_hog_db_bench "q=${q} seq" rcq -n "$n" -w "$WORKERS" -q "$q"
  run_hog_db_bench "q=${q} conc" rcq_concurrent -n "$n" -w "$WORKERS" -q "$q"
done
