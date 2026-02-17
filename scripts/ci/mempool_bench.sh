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

echo 'set port env vars'
export CI_CONTROL_PORT=$(./$BUILD_DIR/hogd/utils/get_port kCIControlServerPort)
export MEMPOOL_AUTH_PORT=$(./$BUILD_DIR/hogd/utils/get_port kMempoolAuthServerPort)
export MEMPOOL_EXECUTOR_PORT=$(./$BUILD_DIR/hogd/utils/get_port kMempoolExecutorServerPort)
export MEMPOOL_DB_PORT=$(./$BUILD_DIR/hogd/utils/get_port kMempoolDbServerPort)
export MEMPOOL_EVENT_PORT=$(./$BUILD_DIR/hogd/utils/get_port kMempoolEventServerPort)
export MEMPOOL_COORDINATOR_PORT=$(./$BUILD_DIR/hogd/utils/get_port kMempoolCoordinatorServerPort)
export EVENT_INDEXER_PORT=$(./$BUILD_DIR/hogd/utils/get_port kEventIndexerServerPort)

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
  ssh -n hog4 "killall scpd" || true
  ssh -n hog4 "killall scpd.dev" || true
  ssh -n hog10 "killall mempool_exec" || true
}
kill_stale_processes

echo 'launch loadgen'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT start hogd/loadgen/loadgen_server --port 11222

echo 'wait for loadgen'
./scripts/wait-port.sh hog7 11222

echo 'launch first hogexec'
# the mempool runs on hog10
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT start hogd/hogexec/hogexec --coordinator-addr hog10:$MEMPOOL_EXECUTOR_PORT --nthreads 1024
# can't wait for hogexec: that's waiting on the coordinator

echo 'launch second hogexec'
# the mempool runs on hog10
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT start hogd/hogexec/hogexec --coordinator-addr hog10:$MEMPOOL_EXECUTOR_PORT --nthreads 1024
# can't wait for hogexec: that's waiting on the coordinator

echo 'check hogexec is running'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT expect_running

echo 'launch event'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT start hogd/event/event_exec --mempool-addr hog10:$MEMPOOL_EVENT_PORT --work-dir $WORK_DIR/ --executor-port $(./$BUILD_DIR/hogd/utils/get_port kEventExecutorServerPort) --mempool-port $(./$BUILD_DIR/hogd/utils/get_port kEventMempoolServerPort) --indexer-port $EVENT_INDEXER_PORT

echo 'launch indexer'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT start hogd/indexer/indexer_exec --event-addr hog9:$EVENT_INDEXER_PORT --db-path $WORK_DIR/ --event-port $EVENT_INDEXER_PORT --block-explorer-port $(./$BUILD_DIR/hogd/utils/get_port kIndexerBlockExplorerServerPort)

echo 'launch hogdb'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT start hogd/hogdb/hogdb --db-path $WORK_DIR/ --ram-bytes 10000000000 --coordinator-addr hog10:$MEMPOOL_DB_PORT --executor-port $(./$BUILD_DIR/hogd/utils/get_port kDbExecutorServerPort) --mempool-port $(./$BUILD_DIR/hogd/utils/get_port kDbMempoolServerPort) --auth-port $(./$BUILD_DIR/hogd/utils/get_port kDbAuthServerPort)

echo 'launch auth'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT start hogd/auth/auth_exec --mempool-addr hog10:$MEMPOOL_AUTH_PORT --db-port $(./$BUILD_DIR/hogd/utils/get_port kAuthDbServerPort) --mempool-port $(./$BUILD_DIR/hogd/utils/get_port kAuthMempoolServerPort)

echo 'launch coordinator and scp'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog4 $CI_CONTROL_PORT start \
  chuckd.dev \
  -l info \
  --skip 20 \
  --stack \
  --config ./scripts/config-chuckd.json \
  --instance 970c9b \
  --seed 1410441218572558875 \
  --metrics :2223 \
  bench coordinator_loadgen \
  --in-addr hog4:7888 \
  --debug-port 5500 \
  --num-accts 10000 \
  --txs-per-block 1000000 \
  --num-blocks 1000 \
  --storage-dir $WORK_DIR/chuckd-storage \
  --out-addr :50000 \
  --wallet-addr :40000 \
  --loadgen-addr hog7:11222 \
  --sched-addr hog10:8070 \
  --scpd-path ./$BUILD_DIR/scpd.dev \
  --scp-handle-addr 127.0.0.1:3300 \
  --scp-control-addr 127.0.0.1:4400

echo 'make sure procs are running'
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog4 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog5 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog6 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog7 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog8 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog9 $CI_CONTROL_PORT expect_running
./$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 $CI_CONTROL_PORT expect_running

echo 'launch mempool'
bash -c 'timeout 180s ./$BUILD_DIR/hogd/mempool/mempool_exec --num-db-shards 1 --num-exec-shards 2 --db-port $MEMPOOL_DB_PORT --event-port $MEMPOOL_EVENT_PORT --executor-port $MEMPOOL_EXECUTOR_PORT --auth-port $MEMPOOL_AUTH_PORT --coordinator-port $MEMPOOL_COORDINATOR_PORT; status=$?; [ $status -eq 124 ] && status=0; exit $status'

echo 'copy coordinator output from hog4'
# We grab the file with the prefix [hogd_stdout] that has the newest timestamp.
scp hog4:"$(ssh -n hog4 'ls -1t /tmp/hogd_stdout* | head -n 1')" ./coordinator_output.txt

echo 'clone metrics history # This is idempotent'
./scripts/clone_metrics_history.sh

echo 'log integration metrics'
./scripts/log_metric.sh nightly_integration_mempool coordinator_output.txt
