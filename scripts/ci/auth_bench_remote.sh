#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

# Runs [auth_bench.sh] on a bench host through its control server, so the
# auth server benchmark can overlap suites that leave that host idle.
#
# Usage: auth_bench_remote.sh start <host>
#        auth_bench_remote.sh collect <host>
#
# [start] launches the benchmark in the host's checkout and returns at once.
# [collect] waits for it, copies its JSON back, and logs auth_server_bench.
# The host's bench_servers job must have built hog_auth_bench, which a
# Release build with BUILD_BENCHMARKS on does.

if [ $# -ne 2 ]; then
  echo "Usage: $0 start|collect <host>" >&2
  exit 2
fi

COMMAND=$1
HOST=$2
BUILD_DIR=${BUILD_DIR:-release}
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
CI_CONTROL_PORT=${CI_CONTROL_PORT:-$("./$BUILD_DIR/hogd/utils/get_port" kCIControlServerPort)}
# Absolute, so [collect] can find it: the control server starts processes in
# its own checkout, whose path this job does not know.
REMOTE_OUTPUT=/tmp/auth_bench
LOCAL_OUTPUT=auth_bench_results
# The sweep takes about two minutes; the slack covers signing the corpus and
# a slow host without letting a hung run hold the job.
WAIT_TIMEOUT_SEC=${AUTH_BENCH_WAIT_TIMEOUT_SEC:-900}

auth_command=(/bin/bash scripts/ci/auth_bench.sh "$BUILD_DIR" "$REMOTE_OUTPUT")
pin_warm_batch64=${AUTH_BENCH_PIN_WARM_BATCH64:-}
place_cold_cases=0
if [[ -n $pin_warm_batch64 ]]; then
  [[ $pin_warm_batch64 == 1 && ${HOGD_CI_CONTAINER:-} == 1 &&
    ${HOGD_CI_OFFLINE:-} == 1 && ${HOG_HOST:-} == hog10 && $HOST == hog9 ]]
elif [[ $COMMAND == start && ( ${GIT_BRANCH:-} == main ||
      ${GIT_BRANCH:-} == codex/auth-cold-all-placement-canary ||
      ${GIT_BRANCH:-} == codex/auth-cold-all-placement-control ) &&
  ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
  ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
  ${HOG_HOST:-} == hog10 && $HOST == hog9 ]]; then
  # Both qualification arms retain the existing warm placement.
  pin_warm_batch64=1
  if [[ $GIT_BRANCH != codex/auth-cold-all-placement-control ]]; then
    place_cold_cases=1
  fi
fi
if [[ $place_cold_cases == 1 ]]; then
  auth_command=(/usr/bin/env AUTH_BENCH_PIN_WARM_BATCH64=1
    AUTH_BENCH_PLACE_COLD_CASES=1 "${auth_command[@]}")
elif [[ $pin_warm_batch64 == 1 ]]; then
  auth_command=(/usr/bin/env AUTH_BENCH_PIN_WARM_BATCH64=1 "${auth_command[@]}")
fi

case $COMMAND in
start)
  ci_wait_control_server "$CONTROL_CLI" "$HOST" "$CI_CONTROL_PORT"
  ci_remote_run "$HOST" "rm -rf $REMOTE_OUTPUT"
  "$CONTROL_CLI" "$HOST" "$CI_CONTROL_PORT" start \
    "${auth_command[@]}"
  ;;
collect)
  status=0
  timeout "$WAIT_TIMEOUT_SEC" \
    "$CONTROL_CLI" "$HOST" "$CI_CONTROL_PORT" wait || status=$?

  echo "copy auth server benchmark output from $HOST"
  rm -rf "$LOCAL_OUTPUT"
  mkdir -p "$LOCAL_OUTPUT"
  # The benchmark's own output goes to the control server's newest stdout and
  # stderr files.
  for stream in stdout stderr; do
    ci_remote_run "$HOST" \
      "cat \"\$(ls -1t /tmp/hogd_${stream}_* | head -n 1)\"" || true
  done
  scp -r -o BatchMode=yes -o ConnectTimeout=5 \
    "$HOST:$REMOTE_OUTPUT/." "$LOCAL_OUTPUT/" || true

  if [ "$status" -ne 0 ]; then
    echo "auth server benchmark on $HOST" \
      "$(ci_describe_exit_status "$status")" >&2
    exit "$status"
  fi
  if [ ! -s "$LOCAL_OUTPUT/auth_bench.json" ]; then
    echo "auth server benchmark on $HOST left no auth_bench.json" >&2
    exit 1
  fi

  echo 'clone metrics history' # This is idempotent
  ./scripts/clone_metrics_history.sh

  echo 'log auth server benchmark metrics'
  ./scripts/log_metric.sh auth_server_bench "$LOCAL_OUTPUT/auth_bench.json"
  ;;
*)
  echo "Unknown command: $COMMAND" >&2
  exit 2
  ;;
esac
