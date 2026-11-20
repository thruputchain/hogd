#!/usr/bin/env bash
set -Eeuo pipefail

# Gateway admission under load: [schedgateway_bench.sh] at eight connections,
# each keeping many calls in flight, so a run measures how much admission the
# mempool takes rather than one call's round trip. hog-devnode gets 32
# threads, which serve hog-mempool and auth's signature checks alike, and the
# client as many.
#
# Usage: schedgateway_stress_bench.sh <output_dir>
#
# Writes <output_dir>/<config>/c8/ per configuration; result.json in each is
# the metric:
#   calls:   64 one-transaction calls in flight on each connection
#   batches: 8 hundred-transaction calls in flight on each connection
# Both configurations run even if the first fails.

if [ $# -ne 1 ]; then
  echo "Usage: $0 <output_dir>" >&2
  exit 2
fi

OUTPUT_ROOT=$1
rm -rf "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT"

export SCHEDGATEWAY_BENCH_CONNECTIONS=8
export SCHEDGATEWAY_BENCH_SECONDS=10
export SCHEDGATEWAY_BENCH_WARMUP=2
export SCHEDGATEWAY_BENCH_WORKERS=32
export SCHEDGATEWAY_BENCH_CLIENT_WORKERS=32
export SCHEDGATEWAY_BENCH_ACCOUNTS=1000

status=0
# name:calls in flight per connection:transactions per call:corpus. A run
# that exhausts its corpus ends early and is measured up to then.
for config in calls:64:1:5000000 batches:8:100:8000000; do
  IFS=: read -r name outstanding batch transactions <<<"$config"
  echo "run schedgateway stress benchmark: $name, 8 connections x" \
    "$outstanding calls x $batch transaction(s)"
  SCHEDGATEWAY_BENCH_OUTSTANDING=$outstanding \
    SCHEDGATEWAY_BENCH_BATCH=$batch \
    SCHEDGATEWAY_BENCH_TRANSACTIONS=$transactions \
    "$(dirname "$0")/schedgateway_bench.sh" "$OUTPUT_ROOT/$name" ||
    status=$?
done
exit "$status"
