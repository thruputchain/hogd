#!/usr/bin/env bash
set -Eeuo pipefail

# The makespan bench at its defaults: block makespans with heavy transactions
# (long fibonacci, 1000 sequential storage reads) mixed into a flood of light
# ones, plus a direct hog-db read probe. Runs on exec_scaling's hosts, which
# are still up when it follows that step. makespan.yml runs the same bench
# with every knob exposed.
CI_WAIT_PORT_TIMEOUT_SEC=${CI_WAIT_PORT_TIMEOUT_SEC:-1000}

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
BENCH_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog11)

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
ci_wait_control_server_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" \
  "${BENCH_HOSTS[@]}"

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

# hog7 event, hog8 db, hog4 indexer, executors hog5, hog11 and hog6: the
# makespan.yml topology at its default of three executors.
echo 'launch makespan runner'
./$BUILD_DIR/hogd/hog_exec/bench/makespan_runner \
  -s hog7 -s hog8 -s hog4 -s hog5 -s hog11 -s hog6 >makespan_runner_log

# The per-trial lines carry the load breakdowns; the series is the config,
# the per-scenario medians and the probe. grep exits 1 on no match, which
# under [set -e] would kill the script before the check below could say why.
grep -E '^(makespan_(config|summary)|probe):' makespan_runner_log \
  >makespan_log || true
if [ "$(grep -c '^makespan_summary:' makespan_log)" -eq 0 ]; then
  echo 'no makespan summaries in runner log' >&2
  exit 1
fi
cat makespan_log

echo 'log makespan metrics'
./scripts/log_metric.sh makespan makespan_log
