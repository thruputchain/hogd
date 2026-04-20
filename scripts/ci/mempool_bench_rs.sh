#!/usr/bin/env bash
set -Eeuo pipefail

cleanup() {
  local exit_code=$1

  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog4 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT stop
  ./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT stop

  kill_stale_processes

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

. "$(dirname "$0")/../hog-ip.sh"

echo 'set port env vars'
export CI_CONTROL_PORT=$(./$BUILD_DIR/hogd/utils/get_port kCIControlServerPort)

export LOADGEN_PORT=11222

export AUTH_DB_PORT=8050
export AUTH_MEMPOOL_PORT=8051

export INDEXER_EVENT_PORT=8060
export INDEXER_BLOCK_EXPLORER_PORT=8061

export MEMPOOL_COORDINATOR_PORT=8070
export MEMPOOL_EXECUTOR_PORT=8071
export MEMPOOL_DB_PORT=8072
export MEMPOOL_EVENT_PORT=8073
export MEMPOOL_AUTH_PORT=8074

export DB_EXECUTOR_PORT=8080
export DB_MEMPOOL_PORT=8081
export DB_AUTH_PORT=8082

export EVENT_EXECUTOR_PORT=8090
export EVENT_MEMPOOL_PORT=8091
export EVENT_INDEXER_PORT=8092

echo 'make sure bench servers are running'
./scripts/wait-port.sh hog4 $CI_CONTROL_PORT
./scripts/wait-port.sh hog5 $CI_CONTROL_PORT
./scripts/wait-port.sh hog6 $CI_CONTROL_PORT
./scripts/wait-port.sh hog7 $CI_CONTROL_PORT
./scripts/wait-port.sh hog8 $CI_CONTROL_PORT
./scripts/wait-port.sh hog9 $CI_CONTROL_PORT
./scripts/wait-port.sh hog11 $CI_CONTROL_PORT

echo 'clear work dirs'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog4 $CI_CONTROL_PORT clear_work_dir
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT clear_work_dir
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT clear_work_dir
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT clear_work_dir
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT clear_work_dir
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT clear_work_dir
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT clear_work_dir

echo 'kill stale processes'
kill_stale_processes() {
  # [ssh] will return a non-zero error code if a process we try to kill does not
  # exist. To prevent this non-zero error code from causing this overall script
  # to fail, we append [|| true] to each line.
  ssh -n hog7 "killall loadgen_server" || true
  ssh -n hog5 "killall hogexec" || true
  ssh -n hog6 "killall hogexec" || true
  ssh -n hog8 "killall auth_exec" || true
  ssh -n hog9 "killall event_exec" || true
  ssh -n hog9 "killall indexer_exec" || true
  ssh -n hog11 "killall hogdb" || true
  ssh -n hog4 "killall chuckd" || true
  ssh -n hog4 "killall chuckd.dev" || true
  ssh -n hog10 "killall mempool_exec" || true
}
kill_stale_processes

echo 'launch loadgen'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT start hogd/loadgen/loadgen_server --port $LOADGEN_PORT

echo 'wait for loadgen'
./scripts/wait-port.sh hog7 $LOADGEN_PORT

echo 'launch first hogexec'
# the mempool runs on hog10
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT start hogd/hogexec/hogexec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog5)" --nthreads 1024
# can't wait for hogexec: that's waiting on the coordinator

echo 'launch second hogexec'
# the mempool runs on hog10
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT start hogd/hogexec/hogexec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog6)" --nthreads 1024
# can't wait for hogexec: that's waiting on the coordinator

echo 'check hogexec is running'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT expect_running

echo 'launch event'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT start hogd/event/event_exec --mempool-addr "$(hog_ip hog10)":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip hog9)" --work-dir $WORK_DIR/ --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT

echo 'launch indexer'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT start hogd/indexer/indexer_exec --event-addr "$(hog_ip hog9)":$EVENT_INDEXER_PORT --db-path $WORK_DIR/ --bind-ip "$(hog_ip hog9)" --event-port $INDEXER_EVENT_PORT --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT

echo 'launch hogdb'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT start hogd/hogdb/hogdb --db-path $WORK_DIR/ --ram-bytes 10000000000 --bind-ip "$(hog_ip hog11)" --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT

echo 'launch auth'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT start hogd/auth/auth_exec --mempool-addr "$(hog_ip hog10)":$MEMPOOL_AUTH_PORT --bind-ip "$(hog_ip hog8)" --db-port $AUTH_DB_PORT --mempool-port $AUTH_MEMPOOL_PORT

echo 'launch coordinator (chuckd-rs)'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog4 $CI_CONTROL_PORT start \
  chuckd.dev \
  --log-level info \
  bench coordinator-loadgen \
  --num-accts 10000 \
  --txs-per-block 1000000 \
  --num-blocks 1000 \
  --skip 20 \
  --loadgen-addr "$(hog_ip hog7)":$LOADGEN_PORT \
  --sched-addr "$(hog_ip hog10)":$MEMPOOL_COORDINATOR_PORT \
  --inside-addr "$(hog_ip hog4)":7888

echo 'make sure procs are running'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog4 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT expect_running

echo 'launch mempool'
bash -c 'timeout 180s ./$BUILD_DIR/hogd/mempool/mempool_exec --bind-ip '"$(hog_ip hog10)"' --network-id 1 --num-db-shards 1 --num-exec-shards 2 --db-port $MEMPOOL_DB_PORT --event-port $MEMPOOL_EVENT_PORT --executor-port $MEMPOOL_EXECUTOR_PORT --auth-port $MEMPOOL_AUTH_PORT --coordinator-port $MEMPOOL_COORDINATOR_PORT; status=$?; [ $status -eq 124 ] && status=0; exit $status'

echo 'copy coordinator output from hog4'
# We grab the file with the prefix [hogd_stdout] that has the newest timestamp.
scp hog4:"$(ssh -n hog4 'ls -1t /tmp/hogd_stdout* | head -n 1')" ./coordinator_output.txt

echo 'clone metrics history # This is idempotent'
./scripts/clone_metrics_history.sh

echo 'log integration metrics'
./scripts/log_metric.sh nightly_integration_mempool coordinator_output.txt
