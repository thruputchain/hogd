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


## mass fibonacci.wasm calls
./metrics_history/scripts/make_metric_file.sh fibonacci-wasmonly $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/fibonacci_wasmonly_bench.sh $BUILD_DIR >metrics_history/metrics/fibonacci-wasmonly/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 10 accounts
./metrics_history/scripts/make_metric_file.sh payments-bench-10 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 10 >metrics_history/metrics/payments-bench-10/$GIT_branch/${GIT_commit_time}_$GIT_commit
