#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  echo 'stop event server'
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 stop || true

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

. "$(dirname "$0")/../hog-ip.sh"

echo 'set port env vars'
export EVENT_EXECUTOR_PORT=8090
export EVENT_MEMPOOL_PORT=8091
export EVENT_INDEXER_PORT=8092

echo 'make sure bench server is running'
ci_wait_port hog11 11111

echo 'clear work dirs'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 clear_work_dir

echo 'launch event server'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 start hogd/event/event_exec --mempool-addr "$(hog_ip hog10)":8095 --bind-ip "$(hog_ip hog11)" --work-dir $WORK_DIR/ --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT

echo 'make sure event server is running'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 expect_running

echo 'wait for event server ports'
ci_wait_port hog11 $EVENT_EXECUTOR_PORT
ci_wait_port hog11 $EVENT_MEMPOOL_PORT

echo 'run event server benchmark'
./$BUILD_DIR/hogd/event/event_server_bench -e "$(hog_ip hog11)":$EVENT_EXECUTOR_PORT -s "$(hog_ip hog11)":$EVENT_MEMPOOL_PORT -n 1000000 -c 40 -o 100 >event_server_bench_log

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'log event server metrics'
./scripts/log_metric.sh event_server_bench event_server_bench_log
