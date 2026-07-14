#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
. "$(dirname "$0")/../hog-ip.sh"

BENCH_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog11)
STALE_PROCESS_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
STALE_PROCESSES="loadgen_server hogexec auth_exec event_exec indexer_exec hogdb chuckd chuckd.dev scpd scpd.dev mempool_exec"
CONTROL_CLI="./$BUILD_DIR/hogd/hogexec/bench/control_cli"

kill_stale_processes() {
  ci_remote_killall_hosts "$STALE_PROCESSES" "${STALE_PROCESS_HOSTS[@]}"
}

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"
  kill_stale_processes

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'set port env vars'
export CI_CONTROL_PORT=$(./$BUILD_DIR/hogd/utils/get_port kCIControlServerPort)

export LOADGEN_PORT=11222
export LOADGEN_HTTP_PORT=11223

export EXECUTOR_MEMPOOL_PORT=8040
export EXECUTOR_HTTP_PORT=8041

export AUTH_DB_PORT=8050
export AUTH_MEMPOOL_PORT=8051
export AUTH_HTTP_PORT=8052

export INDEXER_EVENT_PORT=8060
export INDEXER_BLOCK_EXPLORER_PORT=8061
export INDEXER_HTTP_PORT=8062

export MEMPOOL_COORDINATOR_PORT=8070
export MEMPOOL_EXECUTOR_PORT=8071
export MEMPOOL_DB_PORT=8072
export MEMPOOL_EVENT_PORT=8073
export MEMPOOL_AUTH_PORT=8074
export MEMPOOL_HTTP_PORT=8075

export DB_EXECUTOR_PORT=8080
export DB_MEMPOOL_PORT=8081
export DB_AUTH_PORT=8082
export DB_HTTP_PORT=8083

export EVENT_EXECUTOR_PORT=8090
export EVENT_MEMPOOL_PORT=8091
export EVENT_INDEXER_PORT=8092
export EVENT_HTTP_PORT=8093

echo 'make sure bench servers are running'
for host in "${BENCH_HOSTS[@]}"; do
  ci_wait_port "$host" "$CI_CONTROL_PORT"
done

echo 'clear work dirs'
ci_control_clear_work_dirs "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

echo 'kill stale processes'
kill_stale_processes

echo 'launch loadgen'
$CONTROL_CLI hog7 $CI_CONTROL_PORT start hogd/loadgen/loadgen_server --bind-ip "$(hog_ip hog7)" --port $LOADGEN_PORT --metrics-port $LOADGEN_HTTP_PORT

echo 'wait for loadgen'
ci_wait_port hog7 $LOADGEN_PORT

echo 'launch first hogexec'
# the mempool runs on hog10
$CONTROL_CLI hog5 $CI_CONTROL_PORT start hogd/hogexec/hogexec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog5)" --nthreads 1024 --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hogexec: that's waiting on the coordinator

echo 'launch second hogexec'
# the mempool runs on hog10
$CONTROL_CLI hog6 $CI_CONTROL_PORT start hogd/hogexec/hogexec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog6)" --nthreads 1024 --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hogexec: that's waiting on the coordinator

echo 'check hogexec is running'
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running

echo 'launch event'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/event/event_exec --mempool-addr "$(hog_ip hog10)":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip hog9)" --work-dir $WORK_DIR/ --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT --metrics-port $EVENT_HTTP_PORT

echo 'launch indexer'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/indexer/indexer_exec --event-addr "$(hog_ip hog9)":$EVENT_INDEXER_PORT --db-path $WORK_DIR/ --bind-ip "$(hog_ip hog9)" --event-port $INDEXER_EVENT_PORT --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT --metrics-port $INDEXER_HTTP_PORT

echo 'launch hogdb'
$CONTROL_CLI hog11 $CI_CONTROL_PORT start hogd/hogdb/hogdb --db-path $WORK_DIR/ --ram-bytes 10000000000 --bind-ip "$(hog_ip hog11)" --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT --metrics-port $DB_HTTP_PORT

echo 'launch auth'
$CONTROL_CLI hog8 $CI_CONTROL_PORT start hogd/auth/auth_exec --mempool-addr "$(hog_ip hog10)":$MEMPOOL_AUTH_PORT --bind-ip "$(hog_ip hog8)" --db-port $AUTH_DB_PORT --mempool-port $AUTH_MEMPOOL_PORT --metrics-port $AUTH_HTTP_PORT

echo 'launch coordinator and scp'
$CONTROL_CLI hog4 $CI_CONTROL_PORT start \
  chuckd.dev \
  -l info \
  --skip 20 \
  --stack \
  --config ./scripts/config-chuckd.json \
  --instance 970c9b \
  --seed 1410441218572558875 \
  --metrics :2223 \
  bench coordinator_loadgen \
  --in-addr "$(hog_ip hog4)":7888 \
  --debug-port 5500 \
  --num-accts 10000 \
  --txs-per-block 1000000 \
  --num-blocks 1000 \
  --storage-dir $WORK_DIR/chuckd-storage \
  --out-addr :50000 \
  --loadgen-addr "$(hog_ip hog7)":$LOADGEN_PORT \
  --sched-addr "$(hog_ip hog10)":$MEMPOOL_COORDINATOR_PORT \
  --scpd-path ./$BUILD_DIR/scpd.dev \
  --scp-handle-addr 127.0.0.1:3300 \
  --scp-control-addr 127.0.0.1:4400

echo 'make sure procs are running'
$CONTROL_CLI hog4 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog7 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog8 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog9 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog11 $CI_CONTROL_PORT expect_running

echo 'launch mempool'
bash -c 'timeout 180s ./$BUILD_DIR/hogd/mempool/mempool_exec --bind-ip '"$(hog_ip hog10)"' --metrics-port $MEMPOOL_HTTP_PORT --network-id 1 --num-db-shards 1 --num-exec-shards 2 --db-port $MEMPOOL_DB_PORT --event-port $MEMPOOL_EVENT_PORT --executor-port $MEMPOOL_EXECUTOR_PORT --auth-port $MEMPOOL_AUTH_PORT --coordinator-port $MEMPOOL_COORDINATOR_PORT; status=$?; [ $status -eq 124 ] && status=0; exit $status'

echo 'copy coordinator output from hog4'
# We grab the file with the prefix [hogd_stdout] that has the newest timestamp.
scp hog4:"$(ssh -n hog4 'ls -1t /tmp/hogd_stdout* | head -n 1')" ./coordinator_output.txt

if [ "${CI_SKIP_CLONE_METRICS_HISTORY:-0}" != "1" ]; then
  echo 'clone metrics history # This is idempotent'
  ./scripts/clone_metrics_history.sh
fi

echo 'log integration metrics'
./scripts/log_metric.sh nightly_integration_mempool coordinator_output.txt
