#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

diagnostic=
client_pid=
if [[ ${GIT_BRANCH:-} == codex/event-standard-transport-abba ]]; then
  [[ ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 && ${HOG_HOST:-} == hog10 &&
     ${HOGD_EVENT_NETWORK_DIAGNOSTICS:-} == 1 && ${BUILD_DIR:-} == release &&
     ${HOG_XID:-} =~ ^hogd-event-bench-[1-9][0-9]*-[1-9][0-9]*$ &&
     ${HOGD_EVENT_WORK_DIR:-} == /bench-event/ci_work_dir &&
     ${WORK_DIR%/} == /bench-event/ci_work_dir && ${HOG_DUMP_DIR:-} == /workspace/.ci/dumps &&
     ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" &&
     -r /opt/hog-ci/peer.py ]]
  diagnostic="$HOG_DUMP_DIR/event-standard-transport"
  mkdir -m 700 -- "$diagnostic"
fi

proof=./scripts/ci/event_standard_transport.py
remote_proof() {
  case "$1" in before|after|process|reclaim) ;; *) return 2 ;; esac
  timeout --kill-after=2s 25s python3 /opt/hog-ci/peer.py hog11 \
    "python3 /workspace/scripts/ci/event_standard_transport.py $1" </dev/null
}

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  echo 'stop event server'
  if [[ -n $diagnostic ]]; then
    if [[ -n $client_pid ]]; then
      kill -TERM "$client_pid" 2>/dev/null || true
      wait "$client_pid" 2>/dev/null || true
    fi
    if timeout --kill-after=2s 30s "./$BUILD_DIR/hogd/hog_exec/bench/control_cli" hog11 11111 stop_checked \
       >"$diagnostic/stop.log" 2>&1 &&
       remote_proof reclaim >"$diagnostic/reclaim.json"; then
      if (( exit_code == 0 )); then
        python3 "$proof" validate "$diagnostic" >"$diagnostic/summary.json" || exit_code=1
      fi
    else
      echo 'Event checked stop or isolated scratch reclamation failed' >&2
      (( exit_code != 0 )) || exit_code=1
    fi
  else
    ./$BUILD_DIR/hogd/hog_exec/bench/control_cli hog11 11111 stop || true
  fi

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
ci_wait_control_server ./$BUILD_DIR/hogd/hog_exec/bench/control_cli hog11 11111

echo 'clear work dirs'
./$BUILD_DIR/hogd/hog_exec/bench/control_cli hog11 11111 clear_work_dir

if [[ -n $diagnostic ]]; then
  python3 "$proof" before >"$diagnostic/hog10-before.json"
  remote_proof before >"$diagnostic/hog11-before.json"
fi

echo 'launch event server'
./$BUILD_DIR/hogd/hog_exec/bench/control_cli hog11 11111 start hogd/hog_event/hog-event --mempool-addr "$(hog_ip hog10)":8095 --bind-ip "$(hog_ip hog11)" --work-dir $WORK_DIR/ --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT

echo 'make sure event server is running'
./$BUILD_DIR/hogd/hog_exec/bench/control_cli hog11 11111 expect_running

echo 'wait for event server ports'
ci_wait_port hog11 $EVENT_EXECUTOR_PORT
ci_wait_port hog11 $EVENT_MEMPOOL_PORT

echo 'run event server benchmark'
if [[ -n $diagnostic ]]; then
  remote_proof process >"$diagnostic/hog11-process-before.json"
  timeout --kill-after=3s 240s ./$BUILD_DIR/hogd/hog_event/hog_event_server_bench -e "$(hog_ip hog11)":$EVENT_EXECUTOR_PORT -s "$(hog_ip hog11)":$EVENT_MEMPOOL_PORT -n 1000000 -c 40 -o 100 >event_server_bench_log 2>"$diagnostic/client.stderr" &
  client_pid=$!
  python3 "$proof" process >"$diagnostic/hog10-process.json"
  wait "$client_pid"
  client_pid=
  cp -- event_server_bench_log "$diagnostic/blocks.log"
  remote_proof process >"$diagnostic/hog11-process-after.json"
  python3 "$proof" after >"$diagnostic/hog10-after.json"
  remote_proof after >"$diagnostic/hog11-after.json"
else
  ./$BUILD_DIR/hogd/hog_event/hog_event_server_bench -e "$(hog_ip hog11)":$EVENT_EXECUTOR_PORT -s "$(hog_ip hog11)":$EVENT_MEMPOOL_PORT -n 1000000 -c 40 -o 100 >event_server_bench_log
fi

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'log event server metrics'
./scripts/log_metric.sh event_server_bench event_server_bench_log
