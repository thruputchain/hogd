#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

control_cli="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  echo 'stop event server'
  "$control_cli" hog11 11111 stop || true

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
ci_wait_control_server "$control_cli" hog11 11111

start_event_server() {
  echo 'clear work dirs'
  "$control_cli" hog11 11111 clear_work_dir
  if [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${WORK_DIR%/} == /bench-event/ci_work_dir ]]; then
    ci_remote_run hog11 'rm -rf -- /bench-event/ci_work_dir && mkdir -m 700 /bench-event/ci_work_dir'
  fi

  echo 'launch event server'
  "$control_cli" hog11 11111 start hogd/hog_event/hog-event \
    --mempool-addr "$(hog_ip hog10)":8095 --bind-ip "$(hog_ip hog11)" \
    --work-dir "$WORK_DIR/" --executor-port "$EVENT_EXECUTOR_PORT" \
    --mempool-port "$EVENT_MEMPOOL_PORT" --indexer-port "$EVENT_INDEXER_PORT"
  "$control_cli" hog11 11111 expect_running

  ci_wait_port hog11 "$EVENT_EXECUTOR_PORT"
  ci_wait_port hog11 "$EVENT_MEMPOOL_PORT"
}

run_event_client() {
  "./$BUILD_DIR/hogd/hog_event/hog_event_server_bench" \
    -e "$(hog_ip hog11):$EVENT_EXECUTOR_PORT" -s "$(hog_ip hog11):$EVENT_MEMPOOL_PORT" \
    -n 1000000 -c 40 -o 100 "$@"
}

if [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${HOG_XID:-} == hogd-event-bench-* ]]; then
  results=.ci/dumps/event-nagle
  mkdir -p "$results"
  {
    printf '%s\n' 'nodelay: default executor TCP_NODELAY=1' \
      'nagle: -N sets executor TCP_NODELAY=0'
    printf 'work_dir=%s\n' "$WORK_DIR"
    ci_remote_run hog11 "$(printf 'stat -f -c "filesystem=%%T" -- %q' "$WORK_DIR")"
    echo 'hog10 client compile command'
    ninja -C "$BUILD_DIR" -t commands hog_event_server_bench |
      grep -F ' -c /workspace/hogd/hog_event/event_server_bench.cc'
    echo 'hog11 server compile command'
    ci_remote_run hog11 'ninja -C release -t commands hog-event | grep -F " -c /workspace/hogd/hog_event/hog_event.cc"'
  } >"$results/settings.txt"

  start_event_server
  run_event_client >"$results/nodelay.log" 2>"$results/nodelay.stderr"
  cp "$results/nodelay.log" event_server_bench_log

  "$control_cli" hog11 11111 stop
  start_event_server
  run_event_client -N >"$results/nagle.log" 2>"$results/nagle.stderr"
else
  start_event_server
  run_event_client >event_server_bench_log
fi

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'log event server metrics'
./scripts/log_metric.sh event_server_bench event_server_bench_log
