#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

control_cli="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
jumbo_trial=0
if [[ ${HOGD_CI_CONTAINER:-0} == 1 &&
      ${GIT_BRANCH:-} == codex/event-jumbo-experiment &&
      ${HOG_XID:-} == hogd-e2e-payment-* &&
      ${WORK_DIR:-} == /bench-ram/work_dir/ ]]; then
  jumbo_trial=1
fi

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

if (( jumbo_trial )) ||
   [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${HOG_XID:-} == hogd-event-bench-* ]]; then
  if (( jumbo_trial )); then
    # The payment schedule's after hook shuts down all seven remote servers.
    CI_WAIT_CONTROL_TIMEOUT_SEC="${CI_WAIT_CONTROL_TIMEOUT_SEC:-900}" \
      ci_wait_control_server_hosts "$control_cli" 11111 hog4 hog5 hog6 hog7 hog8 hog9 hog11
  else
    CI_WAIT_CONTROL_TIMEOUT_SEC="${CI_WAIT_CONTROL_TIMEOUT_SEC:-900}" \
      ci_wait_control_server "$control_cli" hog11 11111
  fi

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

  if (( jumbo_trial )); then
    probe='set -euo pipefail
      peer=$(getent ahostsv4 "$1" | awk "NR == 1 {print \$1}")
      [[ $peer =~ ^172\.(24|25|26|27)\. ]]
      route=$(ip -4 -o route get "$peer")
      printf "%s\n" "$route"
      device=$(awk "{for (i=1; i<NF; i++) if (\$i == \"dev\") print \$(i+1)}" <<<"$route")
      link=$(ip -d -o link show dev "$device")
      printf "%s\n" "$link"
      [[ $link == *" mtu 8920 "* && $link =~ (^|[[:space:]])wireguard([[:space:]]|$) ]]
      filesystem=$(stat -f -c %T -- "$2")
      printf "filesystem=%s\n" "$filesystem"
      [[ $filesystem == tmpfs ]]
    '
    mkdir -p -- "$WORK_DIR"
    {
      echo 'hog10'
      bash -c "$probe" bash hog11 "$WORK_DIR"
      echo 'hog11'
      ci_remote_run hog11 "$(printf '%q ' bash -c "$probe" bash hog10 "$WORK_DIR")"
    } >"$results/jumbo-path.txt"
  fi

  start_event_server
  run_event_client >"$results/nodelay.log" 2>"$results/nodelay.stderr"
  cp "$results/nodelay.log" event_server_bench_log

  "$control_cli" hog11 11111 stop
  start_event_server
  run_event_client -N >"$results/nagle.log" 2>"$results/nagle.stderr"
else
  ci_wait_control_server "$control_cli" hog11 11111
  start_event_server
  run_event_client >event_server_bench_log
fi

if (( jumbo_trial )); then
  echo 'Jumbo event experiment results are in .ci/dumps/event-nagle'
  exit 0
fi

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'log event server metrics'
./scripts/log_metric.sh event_server_bench event_server_bench_log
