#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
BENCH_HOSTS=(hog5 hog6 hog7 hog8 hog11)

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  echo 'cleanup control servers'
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'make sure bench servers are running'
for host in "${BENCH_HOSTS[@]}"; do
  ci_wait_port "$host" "$CI_CONTROL_PORT"
done

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'launch multiexec runner one exec'
./$BUILD_DIR/hogd/hog_exec/bench/multiexec_runner -s hog7 -s hog8 -s hog5 -a 100 >multiexec_runner_log

echo 'launch multiexec runner two exec'
./$BUILD_DIR/hogd/hog_exec/bench/multiexec_runner -s hog7 -s hog8 -s hog5 -s hog11 -a 100 >>multiexec_runner_log

echo 'launch multiexec runner three exec'
./$BUILD_DIR/hogd/hog_exec/bench/multiexec_runner -s hog7 -s hog8 -s hog5 -s hog11 -s hog6 -a 100 >>multiexec_runner_log

echo 'log multiexec runner logs'
./scripts/log_metric.sh multiexec_interface multiexec_runner_log
