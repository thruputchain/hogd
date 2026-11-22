#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)
CONTROL_CLI="./${BUILD_DIR}/hogd/hog_exec/bench/control_cli"
if [[ ${GIT_BRANCH:-} == codex/hog-db-storage ]]; then
  [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
     ${HOG_HOST:-} == hog10 && ${HOGD_EVENT_WORK_DIR:-} == /bench-event/ci_work_dir ]]
  ci_wait_control_server "$CONTROL_CLI" hog11 "$CI_CONTROL_PORT"
  timeout --kill-after=15s 1815s python3 /opt/hog-ci/peer.py hog11 \
    'timeout --kill-after=20s 1770s bash ./scripts/ci/hog_db_storage_ab.sh' </dev/null
  exit
fi
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
ci_wait_control_server_hosts "$CONTROL_CLI" "${CI_CONTROL_PORT}" \
  "${BENCH_HOSTS[@]}"

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'run metrics'
./scripts/metrics.sh ${BUILD_DIR}
