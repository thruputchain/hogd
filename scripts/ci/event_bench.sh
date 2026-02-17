#!/usr/bin/env bash
set -Eeuo pipefail

cleanup() {
  local exit_code=$1

  echo 'stop event server'
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 stop

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'set port env vars'
export EVENT_EXECUTOR_PORT=$(./$BUILD_DIR/hogd/utils/get_port kEventExecutorServerPort)
export EVENT_MEMPOOL_PORT=$(./$BUILD_DIR/hogd/utils/get_port kEventMempoolServerPort)

echo 'make sure bench server is running'
./scripts/wait-port.sh hog11 11111

echo 'clear work dirs'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 clear_work_dir

echo 'launch event server'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 start hogd/event/event_exec --mempool-addr hog10:8095 --work-dir $WORK_DIR/ --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $(./$BUILD_DIR/hogd/utils/get_port kEventIndexerServerPort)

echo 'make sure event server is running'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 expect_running

echo 'wait for event server ports'
./scripts/wait-port.sh hog11 $EVENT_EXECUTOR_PORT
./scripts/wait-port.sh hog11 $EVENT_MEMPOOL_PORT

echo 'run event server benchmark'
./$BUILD_DIR/hogd/event/event_server_bench -e hog11:$EVENT_EXECUTOR_PORT -s hog11:$EVENT_MEMPOOL_PORT -n 1000000 -c 40 -o 100 >event_server_bench_log

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'log event server metrics'
./scripts/log_metric.sh event_server_bench event_server_bench_log
