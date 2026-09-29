#!/usr/bin/env bash
set -Eeuo pipefail

# Native schedgateway_submit_tx_batch admission benchmark against a fresh,
# local hog-devnode, at one and four connections. Run from a checkout whose
# [$BUILD_DIR] holds hog-devnode and schedgateway_client_bench. The runner
# owns the node and binds every service to an OS-assigned loopback port, so it
# cannot collide with the control server or the fixed ports other suites use.
#
# Usage: schedgateway_bench.sh <output_dir>
#
# Writes <output_dir>/c<connections>/ per run; result.json in each is the
# metric. [schedgateway_bench_remote.sh] runs this on a bench host.

if [ $# -ne 1 ]; then
  echo "Usage: $0 <output_dir>" >&2
  exit 2
fi

OUTPUT_ROOT=$1
BUILD_DIR=${BUILD_DIR:-release}
SECONDS_MEASURED=${SCHEDGATEWAY_BENCH_SECONDS:-20}
WARMUP_SECONDS=${SCHEDGATEWAY_BENCH_WARMUP:-2}
# The client fails rather than replaying a nonce once its prepared corpus
# runs out, and every connection drains it at its own rate, so the corpus
# must scale with the width. Budget per connection, at ~2x the measured rate.
TRANSACTIONS_PER_CONN=${SCHEDGATEWAY_BENCH_TRANSACTIONS_PER_CONN:-150000}

rm -rf "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT"

# One connection shows per-call admission cost; four show how it scales.
for connections in 1 4; do
  echo "run schedgateway submission benchmark: $connections connection(s)"
  python3 hogd/hog_mempool/test/run_schedgateway_bench.py \
    "$BUILD_DIR" "$OUTPUT_ROOT/c$connections" \
    --connections "$connections" \
    --seconds "$SECONDS_MEASURED" \
    --warmup "$WARMUP_SECONDS" \
    --transactions "$((TRANSACTIONS_PER_CONN * connections))"
done
