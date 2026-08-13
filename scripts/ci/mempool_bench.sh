#!/usr/bin/env bash
set -Eeuo pipefail

: "${CI_WAIT_PORT_TIMEOUT_SEC:=600}"
. "$(dirname "$0")/bench_harness.sh"
. "$(dirname "$0")/../hog-ip.sh"
ci_enable_error_trace
# For [replay_control_log_for_label], which needs [CONTROL_LOG_RECORDS] and
# [remote_run] in scope.
. "$(dirname "$0")/wait_port_report.sh"

BENCH_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog11)
STALE_PROCESS_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
STALE_PROCESSES="loadgen_server hog-exec hog-event hog-indexer hog-db hog-auth hog-mempool chuckd chuckd.dev scpd scpd.dev"
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"

# "<host>|<label>|<stdout path>|<stderr path>" per process launched through a
# control server, for the replay helpers to read.
declare -a CONTROL_LOG_RECORDS=()

remote_run() {
  ci_remote_run "$@"
}

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
ci_wait_port_hosts "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

echo 'clear work dirs'
ci_control_clear_work_dirs "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

echo 'kill stale processes'
kill_stale_processes

echo 'launch loadgen'
$CONTROL_CLI hog7 $CI_CONTROL_PORT start hogd/loadgen/loadgen_server --bind-ip "$(hog_ip hog7)" --port $LOADGEN_PORT --metrics-port $LOADGEN_HTTP_PORT

echo 'wait for loadgen'
ci_wait_port hog7 $LOADGEN_PORT

echo 'launch first hog-exec'
# the mempool runs on hog10
$CONTROL_CLI hog5 $CI_CONTROL_PORT start hogd/hog_exec/hog-exec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog5)" --nthreads 1024 --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hog-exec: that's waiting on the coordinator

echo 'launch second hog-exec'
# the mempool runs on hog10
$CONTROL_CLI hog6 $CI_CONTROL_PORT start hogd/hog_exec/hog-exec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog6)" --nthreads 1024 --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hog-exec: that's waiting on the coordinator

echo 'check hog-exec is running'
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running

echo 'launch event'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/hog_event/hog-event --mempool-addr "$(hog_ip hog10)":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip hog9)" --work-dir $WORK_DIR/ --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT --metrics-port $EVENT_HTTP_PORT

echo 'launch indexer'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/hog_indexer/hog-indexer --event-addr "$(hog_ip hog9)":$EVENT_INDEXER_PORT --db-path $WORK_DIR/ --bind-ip "$(hog_ip hog9)" --event-port $INDEXER_EVENT_PORT --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT --metrics-port $INDEXER_HTTP_PORT

echo 'launch hog-db'
$CONTROL_CLI hog11 $CI_CONTROL_PORT start hogd/hog_db/hog-db --db-path $WORK_DIR/ --ram-bytes 10000000000 --bind-ip "$(hog_ip hog11)" --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT --metrics-port $DB_HTTP_PORT

echo 'launch auth'
$CONTROL_CLI hog8 $CI_CONTROL_PORT start hogd/hog_auth/hog-auth --mempool-addr "$(hog_ip hog10)":$MEMPOOL_AUTH_PORT --bind-ip "$(hog_ip hog8)" --db-port $AUTH_DB_PORT --mempool-port $AUTH_MEMPOOL_PORT --metrics-port $AUTH_HTTP_PORT

echo 'launch coordinator and scp'
# Captured rather than streamed so the log paths [control_cli start] reports can
# be recorded: the coordinator drives this whole benchmark from hog4 and writes
# nothing into this job's log, so a coordinator that dies leaves only its exit
# status behind unless those paths are kept for the replay below.
if ! coordinator_start_output=$($CONTROL_CLI hog4 $CI_CONTROL_PORT start \
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
  --scp-control-addr 127.0.0.1:4400 2>&1); then
  printf '%s\n' "$coordinator_start_output"
  echo 'coordinator launch failed' >&2
  exit 1
fi
printf '%s\n' "$coordinator_start_output"

if ! ci_control_log_paths "$coordinator_start_output" \
  coordinator_stdout_path coordinator_stderr_path; then
  echo 'control_cli start did not report the coordinator log paths' >&2
  exit 1
fi
CONTROL_LOG_RECORDS+=(
  "hog4|coordinator|$coordinator_stdout_path|$coordinator_stderr_path"
)

echo 'make sure procs are running'
$CONTROL_CLI hog4 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog7 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog8 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog9 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog11 $CI_CONTROL_PORT expect_running

echo 'launch mempool'
set +e
timeout 180s ./$BUILD_DIR/hogd/hog_mempool/hog-mempool \
  --bind-ip "$(hog_ip hog10)" \
  --metrics-port "$MEMPOOL_HTTP_PORT" \
  --network-hash 0000000000000000000000000000000000000000000000000000000000000001 \
  --num-db-shards 1 \
  --num-exec-shards 2 \
  --db-port "$MEMPOOL_DB_PORT" \
  --event-port "$MEMPOOL_EVENT_PORT" \
  --executor-port "$MEMPOOL_EXECUTOR_PORT" \
  --auth-port "$MEMPOOL_AUTH_PORT" \
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
mempool_status=$?
set -e

coordinator_status=0
if ! $CONTROL_CLI hog4 "$CI_CONTROL_PORT" expect_running; then
  set +e
  $CONTROL_CLI hog4 "$CI_CONTROL_PORT" wait
  coordinator_status=$?
  set -e
fi

if [[ $coordinator_status -ne 0 ||
  ($mempool_status -ne 0 && $mempool_status -ne 124) ]]; then
  # Whichever of the two died, the coordinator's output is where the reason
  # lives, and it is on hog4. Replay it here, before either exit below, so the
  # failure this suite reports arrives with the evidence for it instead of a
  # bare exit status.
  replay_control_log_for_label coordinator
fi

if [[ $mempool_status -ne 0 && $mempool_status -ne 124 ]]; then
  # 124 is [timeout] reaping a mempool that outlived the load, which is the
  # expected end of this benchmark. Anything else is a real failure, and a
  # signal-terminated status is a crash rather than a reported error, so name
  # the signal instead of leaving a bare number in the log.
  echo "mempool $(ci_describe_exit_status "$mempool_status")" >&2
  exit "$mempool_status"
fi
if [[ $coordinator_status -ne 0 ]]; then
  echo "coordinator $(ci_describe_exit_status "$coordinator_status")" >&2
  exit "$coordinator_status"
fi

# Only the success path needs the file: the metric below is parsed out of it,
# and a failing run has already replayed the same output above. Copying it
# before the exits would let a failed [scp] replace the failure being reported
# with one of its own.
echo 'copy coordinator output from hog4'
scp "hog4:$coordinator_stdout_path" ./coordinator_output.txt

echo 'clone metrics history # This is idempotent'
./scripts/clone_metrics_history.sh

echo 'log integration metrics'
./scripts/log_metric.sh nightly_integration_mempool coordinator_output.txt
