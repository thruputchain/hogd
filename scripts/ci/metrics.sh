#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)
CONTROL_CLI="./${BUILD_DIR}/hogd/hog_exec/bench/control_cli"
BENCH_HOSTS=(hog11 hog8)

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  echo 'stop any running processes'
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM

echo 'make sure bench servers are running'
for host in "${BENCH_HOSTS[@]}"; do
  ci_wait_port "$host" "${CI_CONTROL_PORT}"
done

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'run metrics'
./scripts/metrics.sh ${BUILD_DIR}
