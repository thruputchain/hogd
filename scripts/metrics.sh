#!/bin/sh

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

set -e

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/git_info.sh

BUILD_DIR=$1

# Put metrics to run here

# Codesize metrics
## fibonaci.wasm
./metrics_history/scripts/make_metric_file.sh fibonacci-codesize $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR fibonacci >metrics_history/metrics/fibonacci-codesize/$GIT_branch/${GIT_commit_time}_$GIT_commit

## itc_erc20.wasm
./metrics_history/scripts/make_metric_file.sh erc20-codesize $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR itc_erc20 >metrics_history/metrics/erc20-codesize/$GIT_branch/${GIT_commit_time}_$GIT_commit

## initial_token_claim.wasm
./metrics_history/scripts/make_metric_file.sh initial_token_claim-codesize $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR initial_token_claim >metrics_history/metrics/initial_token_claim-codesize/$GIT_branch/${GIT_commit_time}_$GIT_commit

# Performance metrics
## hogdb_bench
./metrics_history/scripts/make_metric_file.sh hogdb-bench $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench/$GIT_branch/${GIT_commit_time}_$GIT_commit

## mass fibonacci.wasm calls
./metrics_history/scripts/make_metric_file.sh fibonacci-wasmonly $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/fibonacci_wasmonly_bench.sh $BUILD_DIR >metrics_history/metrics/fibonacci-wasmonly/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 10 accounts
./metrics_history/scripts/make_metric_file.sh payments-bench-10 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 10 >metrics_history/metrics/payments-bench-10/$GIT_branch/${GIT_commit_time}_$GIT_commit
