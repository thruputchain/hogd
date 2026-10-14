#!/usr/bin/env bash
set -Eeuo pipefail

# The benchmark control servers build concurrently on remote hosts, and a slow
# checkout (for example, cloning the RocksDB submodule) can delay one well past
# the shared harness's 60-second default. Allow that setup to finish while
# preserving an explicit timeout supplied by the caller.
CI_WAIT_PORT_TIMEOUT_SEC=${CI_WAIT_PORT_TIMEOUT_SEC:-1000}

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
# hog4 hosts the indexer added to each topology below. Exec counts are
# unchanged from before the indexer existed, so the multiexec_interface
# series stays comparable across this change.
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

echo 'launch multiexec runner one exec'
./$BUILD_DIR/hogd/hog_exec/bench/multiexec_runner -s hog7 -s hog8 -s hog4 -s hog5 -a 100 >multiexec_runner_log

echo 'launch multiexec runner two exec'
./$BUILD_DIR/hogd/hog_exec/bench/multiexec_runner -s hog7 -s hog8 -s hog4 -s hog5 -s hog11 -a 100 >>multiexec_runner_log

echo 'launch multiexec runner three exec'
./$BUILD_DIR/hogd/hog_exec/bench/multiexec_runner -s hog7 -s hog8 -s hog4 -s hog5 -s hog11 -s hog6 -a 100 >>multiexec_runner_log

echo 'log multiexec runner logs'
./scripts/log_metric.sh multiexec_interface multiexec_runner_log

# Commit-to-queryable latency as its own series. The probe prints on a stable
# prefix so this extraction does not depend on the surrounding benchmark
# output, and grep -c guards against publishing an empty series when a topology
# change silently stops the indexer from being measured.
echo 'log indexer commit-to-visible latency'
# grep exits 1 on no match, which under [set -e] would kill the script
# before the explicit check below could report why.
grep '^indexer_latency_' multiexec_runner_log >indexer_latency_log || true
if [ "$(grep -c '^indexer_latency_summary:' indexer_latency_log)" -eq 0 ]; then
  echo 'no indexer latency summaries in runner log' >&2
  exit 1
fi
# A summary with no observed blocks, or with committed heights the indexer
# never served, is a broken run, not a fast one. Refuse to publish it as data.
if grep -Eq 'blocks=0[^0-9]|unobserved=[1-9]' indexer_latency_log; then
  echo 'indexer latency summary reports unobserved or zero blocks:' >&2
  grep -E 'blocks=0[^0-9]|unobserved=[1-9]' indexer_latency_log >&2
  exit 1
fi
./scripts/log_metric.sh indexer_commit_latency indexer_latency_log
