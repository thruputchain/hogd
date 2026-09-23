#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

# Native schedgateway_submit_tx_batch admission benchmark against a fresh,
# local hog-devnode. The runner owns the node and binds every service to an
# OS-assigned loopback port, so this needs no bench hosts and cannot collide
# with the control server or the fixed ports the other suites use.

BUILD_DIR=${BUILD_DIR:-release}
RESULTS_DIR=${SCHEDGATEWAY_BENCH_RESULTS_DIR:-schedgateway_bench_results}
SECONDS_MEASURED=${SCHEDGATEWAY_BENCH_SECONDS:-20}
WARMUP_SECONDS=${SCHEDGATEWAY_BENCH_WARMUP:-2}
# The client fails rather than replaying a nonce once its prepared corpus runs
# out; this leaves ample headroom over the measured rate at either width.
TRANSACTIONS=${SCHEDGATEWAY_BENCH_TRANSACTIONS:-300000}

echo 'build benchmark client'
# [schedgateway_client_bench] is EXCLUDE_FROM_ALL, so the job's full build
# does not produce it.
cmake --build "$BUILD_DIR" --parallel "$(nproc)" \
  --target schedgateway_client_bench hog-devnode

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

rm -rf "$RESULTS_DIR"
mkdir -p "$RESULTS_DIR"

# One connection shows per-call admission cost; four show how it scales.
for connections in 1 4; do
  output="$RESULTS_DIR/c$connections"
  echo "run schedgateway submission benchmark: $connections connection(s)"
  python3 hogd/hog_mempool/test/run_schedgateway_bench.py \
    "$BUILD_DIR" "$output" \
    --connections "$connections" \
    --seconds "$SECONDS_MEASURED" \
    --warmup "$WARMUP_SECONDS" \
    --transactions "$TRANSACTIONS"

  echo "log schedgateway submission metrics: $connections connection(s)"
  ./scripts/log_metric.sh "schedgateway_submit_c$connections" \
    "$output/result.json"
done
