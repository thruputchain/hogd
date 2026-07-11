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
./scripts/make_metric_file.sh fibonacci-codesize $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR fibonacci >metrics_history/metrics/fibonacci-codesize/$GIT_branch/${GIT_commit_time}_$GIT_commit

## itc_erc20.wasm
./scripts/make_metric_file.sh erc20-codesize $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR itc_erc20 >metrics_history/metrics/erc20-codesize/$GIT_branch/${GIT_commit_time}_$GIT_commit

## initial_token_claim.wasm
./scripts/make_metric_file.sh initial_token_claim-codesize $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR initial_token_claim >metrics_history/metrics/initial_token_claim-codesize/$GIT_branch/${GIT_commit_time}_$GIT_commit

# RV32 codesize metrics: size of the on-chain .rv32a artifact for the same
# contracts measured above for wasm.
## fibonacci.rv32a
./scripts/make_metric_file.sh fibonacci-codesize-rv32 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR fibonacci rv32a >metrics_history/metrics/fibonacci-codesize-rv32/$GIT_branch/${GIT_commit_time}_$GIT_commit

## itc_erc20.rv32a
./scripts/make_metric_file.sh erc20-codesize-rv32 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR itc_erc20 rv32a >metrics_history/metrics/erc20-codesize-rv32/$GIT_branch/${GIT_commit_time}_$GIT_commit

## initial_token_claim.rv32a
./scripts/make_metric_file.sh initial_token_claim-codesize-rv32 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR initial_token_claim rv32a >metrics_history/metrics/initial_token_claim-codesize-rv32/$GIT_branch/${GIT_commit_time}_$GIT_commit

# Performance metrics
## hogdb_bench (roundrobin_payments)
./scripts/make_metric_file.sh hogdb-bench $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hogdb_bench_onetomany
./scripts/make_metric_file.sh hogdb-bench-onetomany $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench_onetomany.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench-onetomany/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hogdb_bench_manytoone
./scripts/make_metric_file.sh hogdb-bench-manytoone $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench_manytoone.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench-manytoone/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hogdb_bench_new_storage
./scripts/make_metric_file.sh hogdb-bench-new-storage $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench_new_storage.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench-new-storage/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hogdb_bench_rcq (sequential insert+consume and concurrent insert+consume)
./scripts/make_metric_file.sh hogdb-bench-rcq $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench_rcq.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench-rcq/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hogdb_bench_rcq_sweep (queue-count sweep)
./scripts/make_metric_file.sh hogdb-bench-rcq-sweep $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hogdb_bench_rcq_sweep.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench-rcq-sweep/$GIT_branch/${GIT_commit_time}_$GIT_commit

## mass fibonacci.wasm calls
./scripts/make_metric_file.sh fibonacci-wasmonly $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/fibonacci_wasmonly_bench.sh $BUILD_DIR >metrics_history/metrics/fibonacci-wasmonly/$GIT_branch/${GIT_commit_time}_$GIT_commit

## mass fibonacci.rv32a calls (same workload as fibonacci-wasmonly, RV32 runtime)
./scripts/make_metric_file.sh fibonacci-rv32only $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/fibonacci_rv32only_bench.sh $BUILD_DIR >metrics_history/metrics/fibonacci-rv32only/$GIT_branch/${GIT_commit_time}_$GIT_commit

## rv32 vm benchmarks (canonical + direct-threaded interpreter: fibonacci, compute loops, wasmi/artifact comparisons)
./scripts/make_metric_file.sh rv32-vm-bench $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/rv32_vm_bench.sh $BUILD_DIR >metrics_history/metrics/rv32-vm-bench/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 10 accounts
./scripts/make_metric_file.sh payments-bench-10 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 10 >metrics_history/metrics/payments-bench-10/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 2 accounts
./scripts/make_metric_file.sh payments-bench-2 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 2 >metrics_history/metrics/payments-bench-2/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 10 accounts, rv32 contracts (same workload as payments-bench-10, RV32 runtime)
./scripts/make_metric_file.sh payments-bench-rv32-10 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 10 rv32 >metrics_history/metrics/payments-bench-rv32-10/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 2 accounts, rv32 contracts (same workload as payments-bench-2, RV32 runtime)
./scripts/make_metric_file.sh payments-bench-rv32-2 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 2 rv32 >metrics_history/metrics/payments-bench-rv32-2/$GIT_branch/${GIT_commit_time}_$GIT_commit

## tx size stats
./scripts/make_metric_file.sh tx-size-stats $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/txsize.sh $BUILD_DIR >metrics_history/metrics/tx-size-stats/$GIT_branch/${GIT_commit_time}_$GIT_commit

## gas usage metric
./scripts/make_metric_file.sh gas-usage-metric $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/gas_usage_metric.sh $BUILD_DIR >metrics_history/metrics/gas-usage-metric/$GIT_branch/${GIT_commit_time}_$GIT_commit

## artifact sizes
./scripts/make_metric_file.sh artifact-sizes $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/artifact_sizes.sh $BUILD_DIR >metrics_history/metrics/artifact-sizes/$GIT_branch/${GIT_commit_time}_$GIT_commit

## x23 metrics
./scripts/make_metric_file.sh x23-metrics $GIT_branch $GIT_commit $GIT_commit_time
${BUILD_DIR:-.}/x23/tests/bench >metrics_history/metrics/x23-metrics/$GIT_branch/${GIT_commit_time}_$GIT_commit
