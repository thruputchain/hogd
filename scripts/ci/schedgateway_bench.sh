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
# SCHEDGATEWAY_BENCH_CONNECTIONS lists the connection counts to run, space
# separated; it defaults to "1 4". SCHEDGATEWAY_BENCH_OUTSTANDING (calls in
# flight per connection), SCHEDGATEWAY_BENCH_BATCH (transactions per call),
# SCHEDGATEWAY_BENCH_CLIENT_WORKERS, SCHEDGATEWAY_BENCH_ACCOUNTS and
# SCHEDGATEWAY_BENCH_WORKERS (hog-devnode's threads) default to one call of
# one transaction at a time, on eight client threads, over 100 accounts,
# against eight node threads. SCHEDGATEWAY_BENCH_TRANSACTIONS fixes the
# corpus.
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
# A run that exhausts its prepared corpus ends early, measured up to then,
# and every connection drains it at its own rate, so the corpus must scale
# with the width. Budget per connection, at ~2x the measured rate.
TRANSACTIONS_PER_CONN=${SCHEDGATEWAY_BENCH_TRANSACTIONS_PER_CONN:-150000}
CONNECTIONS=${SCHEDGATEWAY_BENCH_CONNECTIONS:-1 4}
OUTSTANDING=${SCHEDGATEWAY_BENCH_OUTSTANDING:-1}
BATCH=${SCHEDGATEWAY_BENCH_BATCH:-1}
CLIENT_WORKERS=${SCHEDGATEWAY_BENCH_CLIENT_WORKERS:-8}
ACCOUNTS=${SCHEDGATEWAY_BENCH_ACCOUNTS:-100}
# hog-devnode's threads, which serve the built-in mempool and auth alike.
WORKERS=${SCHEDGATEWAY_BENCH_WORKERS:-8}

rm -rf "$OUTPUT_ROOT"
mkdir -p "$OUTPUT_ROOT"

# The client prepares at most a million txs unless told the corpus.
transactions() {
  if [ -n "${SCHEDGATEWAY_BENCH_TRANSACTIONS:-}" ]; then
    echo "$SCHEDGATEWAY_BENCH_TRANSACTIONS"
    return
  fi
  local total=$((TRANSACTIONS_PER_CONN * $1))
  echo $((total > 1000000 ? 1000000 : total))
}

# One connection shows per-call admission cost; more show how it scales.
for connections in $CONNECTIONS; do
  echo "run schedgateway submission benchmark: $connections connection(s)"
  python3 hogd/hog_mempool/test/run_schedgateway_bench.py \
    "$BUILD_DIR" "$OUTPUT_ROOT/c$connections" \
    --connections "$connections" \
    --seconds "$SECONDS_MEASURED" \
    --warmup "$WARMUP_SECONDS" \
    --transactions "$(transactions "$connections")" \
    --outstanding "$OUTSTANDING" \
    --batch "$BATCH" \
    --client-workers "$CLIENT_WORKERS" \
    --accounts "$ACCOUNTS" \
    --workers "$WORKERS"
done
