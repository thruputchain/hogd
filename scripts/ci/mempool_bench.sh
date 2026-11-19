#!/usr/bin/env bash
set -Eeuo pipefail

: "${CI_WAIT_PORT_TIMEOUT_SEC:=600}"
. "$(dirname "$0")/bench_harness.sh"
. "$(dirname "$0")/../hog-ip.sh"
ci_enable_error_trace

BENCH_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog11)
STALE_PROCESS_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
# The mempool under test. A drop-in replacement that takes hog-mempool's flags
# is benchmarked by pointing HOGD_MEMPOOL_BINARY at it; thruputchain/benchmarks
# builds one beside this checkout.
MEMPOOL_BINARY="${HOGD_MEMPOOL_BINARY:-./$BUILD_DIR/hogd/hog_mempool/hog-mempool}"
STALE_PROCESSES="loadgen_server hog-exec hog-event hog-indexer hog-db hog-auth hog-mempool chuckd chuckd.dev scpd scpd.dev"
if [[ "$(basename "$MEMPOOL_BINARY")" != hog-mempool ]]; then
  STALE_PROCESSES+=" $(basename "$MEMPOOL_BINARY")"
fi
CONTROL_CLI="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
WORK_ROOT=$([[ $WORK_DIR = /* ]] && printf '%s' "${WORK_DIR%/}" || printf '%s/%s' "$PWD" "${WORK_DIR%/}")
EVENT_WORK_DIR="$WORK_ROOT/event"
INDEXER_DB_DIR="$WORK_ROOT/indexer"

kill_stale_processes() {
  ci_remote_killall_hosts "$STALE_PROCESSES" "${STALE_PROCESS_HOSTS[@]}"
}

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"
  kill_stale_processes

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM # 130 is conventional for SIGINT

echo 'set port env vars'
export CI_CONTROL_PORT=$(./$BUILD_DIR/hogd/utils/get_port kCIControlServerPort)

export LOADGEN_PORT=11222
export LOADGEN_HTTP_PORT=11223

export EXECUTOR_MEMPOOL_PORT=8040
export EXECUTOR_HTTP_PORT=8041

export AUTH_DB_PORT=8050
export AUTH_MEMPOOL_PORT=8051
export AUTH_HTTP_PORT=8052

export INDEXER_BLOCK_EXPLORER_PORT=8061
export INDEXER_HTTP_PORT=8062

export MEMPOOL_COORDINATOR_PORT=8070
export MEMPOOL_EXECUTOR_PORT=8071
export MEMPOOL_DB_PORT=8072
export MEMPOOL_EVENT_PORT=8073
export MEMPOOL_AUTH_PORT=8074
export MEMPOOL_HTTP_PORT=8075

export DB_EXECUTOR_PORT=8080
export DB_MEMPOOL_PORT=8081
export DB_AUTH_PORT=8082
export DB_HTTP_PORT=8083

export EVENT_EXECUTOR_PORT=8090
export EVENT_MEMPOOL_PORT=8091
export EVENT_INDEXER_PORT=8092
export EVENT_HTTP_PORT=8093

echo 'make sure bench servers are running'
ci_wait_control_server_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" \
  "${BENCH_HOSTS[@]}"

echo 'clear work dirs'
ci_control_clear_work_dirs "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

echo 'kill stale processes'
kill_stale_processes

echo 'create event and indexer work dirs'
ci_remote_run hog9 "mkdir -p $(printf '%q' "$EVENT_WORK_DIR") $(printf '%q' "$INDEXER_DB_DIR")"

echo 'launch loadgen'
$CONTROL_CLI hog7 $CI_CONTROL_PORT start hogd/loadgen/loadgen_server --bind-ip "$(hog_ip hog7)" --port $LOADGEN_PORT --metrics-port $LOADGEN_HTTP_PORT

echo 'wait for loadgen'
ci_wait_port hog7 $LOADGEN_PORT

echo 'launch first hog-exec'
# the mempool runs on hog10
$CONTROL_CLI hog5 $CI_CONTROL_PORT start hogd/hog_exec/hog-exec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog5)" --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hog-exec: that's waiting on the coordinator

echo 'launch second hog-exec'
# the mempool runs on hog10
$CONTROL_CLI hog6 $CI_CONTROL_PORT start hogd/hog_exec/hog-exec --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip hog6)" --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
# can't wait for hog-exec: that's waiting on the coordinator

echo 'check hog-exec is running'
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running

echo 'launch event'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/hog_event/hog-event --mempool-addr "$(hog_ip hog10)":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip hog9)" --work-dir "$EVENT_WORK_DIR" --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT --metrics-port $EVENT_HTTP_PORT

echo 'launch indexer'
$CONTROL_CLI hog9 $CI_CONTROL_PORT start hogd/hog_indexer/hog-indexer --event-addr "$(hog_ip hog9)":$EVENT_INDEXER_PORT --db-path "$INDEXER_DB_DIR" --bind-ip "$(hog_ip hog9)" --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT --metrics-port $INDEXER_HTTP_PORT

echo 'launch hog-db'
$CONTROL_CLI hog11 $CI_CONTROL_PORT start hogd/hog_db/hog-db --db-path $WORK_DIR/ --ram-bytes 10000000000 --bind-ip "$(hog_ip hog11)" --coordinator-addr "$(hog_ip hog10)":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT --metrics-port $DB_HTTP_PORT

echo 'launch auth'
$CONTROL_CLI hog8 $CI_CONTROL_PORT start hogd/hog_auth/hog-auth --mempool-addr "$(hog_ip hog10)":$MEMPOOL_AUTH_PORT --bind-ip "$(hog_ip hog8)" --db-port $AUTH_DB_PORT --mempool-port $AUTH_MEMPOOL_PORT --metrics-port $AUTH_HTTP_PORT

echo 'launch coordinator and scp'
$CONTROL_CLI hog4 $CI_CONTROL_PORT start \
  chuckd.dev \
  -l info \
  --stack \
  --config ./scripts/config-chuckd.json \
  --instance 970c9b \
  --seed 1410441218572558875 \
  --metrics :2223 \
  bench coordinator_loadgen \
  --skip 20 \
  --in-addr "$(hog_ip hog4)":7888 \
  --debug-port 5500 \
  --num-accts 10000 \
  --txs-per-block 1000000 \
  --num-blocks 1000 \
  --network-hash 11111111111111111111111111111112 \
  --storage-dir $WORK_DIR/chuckd-storage \
  --out-addr :50000 \
  --loadgen-addr "$(hog_ip hog7)":$LOADGEN_PORT \
  --sched-addr "$(hog_ip hog10)":$MEMPOOL_COORDINATOR_PORT \
  --scpd-path ./$BUILD_DIR/scpd.dev \
  --scp-handle-addr 127.0.0.1:3300 \
  --scp-control-addr 127.0.0.1:4400

echo 'make sure procs are running'
$CONTROL_CLI hog4 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog5 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog6 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog7 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog8 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog9 $CI_CONTROL_PORT expect_running
$CONTROL_CLI hog11 $CI_CONTROL_PORT expect_running

# The mempool's own timings are only on its metrics port, and `timeout` below
# reaps it at 180 s, so take a snapshot shortly before then.
rm -f mempool_metrics.txt
(
  sleep 170
  curl --silent --max-time 5 "http://$(hog_ip hog10):$MEMPOOL_HTTP_PORT/metrics" \
    > mempool_metrics.txt || true
) &

# hog-mempool writes each submission's DAG, with per-node timings, here.
export MEMPOOL_DAG_DOT_DIR="$WORK_ROOT/submit_dags"
rm -rf "$MEMPOOL_DAG_DOT_DIR"
mkdir -p "$MEMPOOL_DAG_DOT_DIR"

echo "launch mempool ($MEMPOOL_BINARY)"
set +e
timeout 180s "$MEMPOOL_BINARY" \
  --bind-ip "$(hog_ip hog10)" \
  --metrics-port "$MEMPOOL_HTTP_PORT" \
  --network-hash 11111111111111111111111111111112 \
  --num-db-shards 1 \
  --num-exec-shards 2 \
  --db-port "$MEMPOOL_DB_PORT" \
  --event-port "$MEMPOOL_EVENT_PORT" \
  --executor-port "$MEMPOOL_EXECUTOR_PORT" \
  --auth-port "$MEMPOOL_AUTH_PORT" \
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
mempool_status=$?
set -e

coordinator_status=0
if ! $CONTROL_CLI hog4 "$CI_CONTROL_PORT" expect_running; then
  set +e
  $CONTROL_CLI hog4 "$CI_CONTROL_PORT" wait
  coordinator_status=$?
  set -e
fi

echo 'copy coordinator output from hog4'
# We grab the file with the prefix [hogd_stdout] that has the newest timestamp.
scp hog4:"$(ssh -n hog4 'ls -1t /tmp/hogd_stdout* | head -n 1')" ./coordinator_output.txt

if [[ $mempool_status -ne 0 && $mempool_status -ne 124 ]]; then
  # 124 is [timeout] reaping a mempool that outlived the load, which is the
  # expected end of this benchmark. Anything else is a real failure, and a
  # signal-terminated status is a crash rather than a reported error, so name
  # the signal instead of leaving a bare number in the log.
  echo "mempool $(ci_describe_exit_status "$mempool_status")" >&2
  exit "$mempool_status"
fi
if [[ $coordinator_status -ne 0 ]]; then
  echo "coordinator $(ci_describe_exit_status "$coordinator_status")" >&2
  exit "$coordinator_status"
fi

echo 'coordinator benchmark results'
cat coordinator_output.txt

echo 'mempool metrics snapshot'
# Counters, sums and gauges, plus the buckets of the per-call timings whose
# means mix full and empty blocks.
grep -v -E '^#|_bucket\{' mempool_metrics.txt || echo 'no mempool metrics snapshot'
grep -E '^mempool_(nomination_duration|commit_duration|ingestion_auth_rpc|submit_unvetted_duration)_seconds_bucket' \
  mempool_metrics.txt || true

echo 'submit dag timings'
# Median wait and run per node over every submission, from hog-mempool's
# DOT dumps (the coordinator pumps only batches of a million).
python3 - "$MEMPOOL_DAG_DOT_DIR" <<'PY' || echo 'no submit dag timings'
import glob, re, statistics, sys
def secs(v):
    m = re.match(r'([\d.]+)(us|ms|s)$', v)
    return float(m.group(1)) * {'us': 1e-6, 'ms': 1e-3, 's': 1.0}[m.group(2)]
rows = {}
files = sorted(glob.glob(sys.argv[1] + '/mempool_submit_unvetted_*.dot'))
for path in files:
    text = open(path).read()
    for name, body in re.findall(r'label="\d+: ([^\\"]+)((?:\\n[^"]*)?)"', text):
        fields = dict(re.findall(r'\\n(wait|run|total): ([\d.]+(?:us|ms|s))', body))
        if 'run' in fields:
            rows.setdefault(name, []).append((secs(fields['wait']), secs(fields['run'])))
print(f'{len(files)} submit dags')
for name, v in rows.items():
    waits = [w for w, _ in v]
    runs = [r for _, r in v]
    print(f'{name:28s} n={len(v):4d} wait_med={statistics.median(waits)*1e3:8.1f}ms '
          f'run_med={statistics.median(runs)*1e3:8.1f}ms run_max={max(runs)*1e3:8.1f}ms')
PY

echo 'auth rpc profile'
# hog-auth logs to its control server's newest stderr file on hog8.
ssh -n hog8 'grep -h RPCPROF "$(ls -1t /tmp/hogd_stderr_* | head -n 1)" "$(ls -1t /tmp/hogd_stdout_* | head -n 1)" | tail -n 8' \
  || echo 'no auth rpc profile'

echo 'clone metrics history # This is idempotent'
./scripts/clone_metrics_history.sh

echo 'log integration metrics'
./scripts/log_metric.sh "${CI_MEMPOOL_METRIC_NAME:-nightly_integration_mempool}" coordinator_output.txt
