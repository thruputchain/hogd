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

# Codesize metrics: size of the on-chain .rv32a artifact image.
## fibonacci.rv32a
./scripts/make_metric_file.sh fibonacci-codesize-rv32 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR fibonacci >metrics_history/metrics/fibonacci-codesize-rv32/$GIT_branch/${GIT_commit_time}_$GIT_commit

## itc_erc20.rv32a
./scripts/make_metric_file.sh erc20-codesize-rv32 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/codesize.sh $BUILD_DIR itc_erc20 >metrics_history/metrics/erc20-codesize-rv32/$GIT_branch/${GIT_commit_time}_$GIT_commit

# Performance metrics
## hog_db_bench (roundrobin_payments)
./scripts/make_metric_file.sh hog-db-bench $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hog_db_bench.sh $BUILD_DIR >metrics_history/metrics/hog-db-bench/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hog_db_bench_onetomany
./scripts/make_metric_file.sh hog-db-bench-onetomany $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hog_db_bench_onetomany.sh $BUILD_DIR >metrics_history/metrics/hog-db-bench-onetomany/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hog_db_bench_manytoone
./scripts/make_metric_file.sh hog-db-bench-manytoone $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hog_db_bench_manytoone.sh $BUILD_DIR >metrics_history/metrics/hog-db-bench-manytoone/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hog_db_bench_new_storage
./scripts/make_metric_file.sh hog-db-bench-new-storage $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hog_db_bench_new_storage.sh $BUILD_DIR >metrics_history/metrics/hog-db-bench-new-storage/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hog_db_bench_rcq (sequential insert+consume and concurrent insert+consume)
./scripts/make_metric_file.sh hog-db-bench-rcq $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hog_db_bench_rcq.sh $BUILD_DIR >metrics_history/metrics/hog-db-bench-rcq/$GIT_branch/${GIT_commit_time}_$GIT_commit

## hog_db_bench_rcq_sweep (queue-count sweep)
./scripts/make_metric_file.sh hog-db-bench-rcq-sweep $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/hog_db_bench_rcq_sweep.sh $BUILD_DIR >metrics_history/metrics/hog-db-bench-rcq-sweep/$GIT_branch/${GIT_commit_time}_$GIT_commit

## mass fibonacci.rv32a calls
./scripts/make_metric_file.sh fibonacci-rv32only $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/fibonacci_rv32only_bench.sh $BUILD_DIR >metrics_history/metrics/fibonacci-rv32only/$GIT_branch/${GIT_commit_time}_$GIT_commit

## rv32 vm benchmarks (canonical + direct-threaded interpreter: fibonacci, compute loops, artifact benchmarks)
./scripts/make_metric_file.sh rv32-vm-bench $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/rv32_vm_bench.sh $BUILD_DIR >metrics_history/metrics/rv32-vm-bench/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 10 accounts
./scripts/make_metric_file.sh payments-bench-rv32-10 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 10 >metrics_history/metrics/payments-bench-rv32-10/$GIT_branch/${GIT_commit_time}_$GIT_commit

## payments_bench 2 accounts
./scripts/make_metric_file.sh payments-bench-rv32-2 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 2 >metrics_history/metrics/payments-bench-rv32-2/$GIT_branch/${GIT_commit_time}_$GIT_commit

## tx size stats
./scripts/make_metric_file.sh tx-size-stats $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/txsize.sh $BUILD_DIR >metrics_history/metrics/tx-size-stats/$GIT_branch/${GIT_commit_time}_$GIT_commit

## gas usage metric
./scripts/make_metric_file.sh gas-usage-metric $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/gas_usage_metric.sh $BUILD_DIR >metrics_history/metrics/gas-usage-metric/$GIT_branch/${GIT_commit_time}_$GIT_commit

## rv32 artifact sizes (per-contract .rv32a size across the built contract set)
./scripts/make_metric_file.sh artifact-sizes-rv32 $GIT_branch $GIT_commit $GIT_commit_time
./scripts/metrics/artifact_sizes.sh $BUILD_DIR >metrics_history/metrics/artifact-sizes-rv32/$GIT_branch/${GIT_commit_time}_$GIT_commit

## x23 metrics
./scripts/make_metric_file.sh x23-metrics $GIT_branch $GIT_commit $GIT_commit_time
${BUILD_DIR:-.}/x23/tests/bench >metrics_history/metrics/x23-metrics/$GIT_branch/${GIT_commit_time}_$GIT_commit
