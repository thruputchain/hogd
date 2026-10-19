#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

# Runs [schedgateway_bench.sh] and then [schedgateway_stress_bench.sh] on a
# bench host through its control server, so the benchmarks can overlap suites
# that leave that host idle.
#
# Usage: schedgateway_bench_remote.sh start <host>
#        schedgateway_bench_remote.sh collect <host>
#
# [start] launches the benchmark in the host's checkout and returns at once.
# [collect] waits for it, copies its results back, and logs
# schedgateway_submit_c1 and schedgateway_submit_c4 (one call in flight per
# connection) and schedgateway_stress_calls and schedgateway_stress_batches
# (many). The host's bench_servers job must have built
# schedgateway_client_bench, which is EXCLUDE_FROM_ALL.

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
REMOTE_OUTPUT=/tmp/schedgateway_bench
REMOTE_STRESS_OUTPUT=/tmp/schedgateway_stress_bench
LOCAL_OUTPUT=schedgateway_bench_results
LOCAL_STRESS_OUTPUT=schedgateway_stress_bench_results
# Covers four node startups, corpus signing, and all four measured runs. Keep
# it above the sum of the runs' own client timeouts (about 210 s, 240 s,
# 490 s and 670 s at the defaults) so a stuck client reports itself rather
# than being killed here.
WAIT_TIMEOUT_SEC=${SCHEDGATEWAY_BENCH_WAIT_TIMEOUT_SEC:-2400}

case $COMMAND in
start)
  ci_wait_control_server "$CONTROL_CLI" "$HOST" "$CI_CONTROL_PORT"
  # The stress runs follow the one-call runs whether or not those pass; the
  # pair fails if either does.
  "$CONTROL_CLI" "$HOST" "$CI_CONTROL_PORT" start \
    /bin/bash -c 'scripts/ci/schedgateway_bench.sh "$1"; s=$?; scripts/ci/schedgateway_stress_bench.sh "$2" || s=$?; exit "$s"' \
    schedgateway_bench "$REMOTE_OUTPUT" "$REMOTE_STRESS_OUTPUT"
  ;;
collect)
  status=0
  timeout "$WAIT_TIMEOUT_SEC" \
    "$CONTROL_CLI" "$HOST" "$CI_CONTROL_PORT" wait || status=$?

  echo "copy schedgateway benchmark output from $HOST"
  rm -rf "$LOCAL_OUTPUT" "$LOCAL_STRESS_OUTPUT"
  # The benchmark's own output goes to the control server's newest stdout and
  # stderr files; its run directories hold the node and client logs.
  mkdir -p "$LOCAL_OUTPUT" "$LOCAL_STRESS_OUTPUT"
  for stream in stdout stderr; do
    ci_remote_run "$HOST" \
      "cat \"\$(ls -1t /tmp/hogd_${stream}_* | head -n 1)\"" || true
  done
  scp -r -o BatchMode=yes -o ConnectTimeout=5 \
    "$HOST:$REMOTE_OUTPUT/." "$LOCAL_OUTPUT/" || true
  scp -r -o BatchMode=yes -o ConnectTimeout=5 \
    "$HOST:$REMOTE_STRESS_OUTPUT/." "$LOCAL_STRESS_OUTPUT/" || true

  if [ "$status" -ne 0 ]; then
    echo "schedgateway benchmark on $HOST" \
      "$(ci_describe_exit_status "$status")" >&2
    for log in "$LOCAL_OUTPUT"/c*/{client,node}.log \
      "$LOCAL_STRESS_OUTPUT"/*/c*/{client,node}.log; do
      [ -f "$log" ] || continue
      echo "--- $log"
      # hog-devnode prints a rejected launch's "error:" line ahead of a usage
      # text that alone fills the tail, so show those lines first.
      grep -m 5 '^error:' "$log" || true
      tail -n 50 "$log"
    done
    exit "$status"
  fi

  echo 'clone metrics history' # This is idempotent
  ./scripts/clone_metrics_history.sh

  for connections in 1 4; do
    echo "log schedgateway submission metrics: $connections connection(s)"
    ./scripts/log_metric.sh "schedgateway_submit_c$connections" \
      "$LOCAL_OUTPUT/c$connections/result.json"
  done
  for config in calls batches; do
    echo "log schedgateway stress metrics: $config"
    ./scripts/log_metric.sh "schedgateway_stress_$config" \
      "$LOCAL_STRESS_OUTPUT/$config/c8/result.json"
  done
  ;;
*)
  echo "Unknown command: $COMMAND" >&2
  exit 2
  ;;
esac
