#!/usr/bin/env bash
set -Eeuo pipefail

: "${BUILD_DIR:?BUILD_DIR must be set}"
: "${WORK_DIR:?WORK_DIR must be set}"
: "${CI_WAIT_PORT_TIMEOUT_SEC:=600}"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
. "$SCRIPT_DIR/bench_harness.sh"
ci_enable_error_trace
. "$SCRIPT_DIR/wait_port_report.sh"
WAIT_PORT=wait_port_or_report
BUILD_ROOT=$([[ $BUILD_DIR = /* ]] && printf '%s' "$BUILD_DIR" || printf '%s' "$REPO_ROOT/$BUILD_DIR")
WORK_ROOT=$([[ $WORK_DIR = /* ]] && printf '%s' "$WORK_DIR" || printf '%s' "$REPO_ROOT/$WORK_DIR")
BENCH_ROOT="$WORK_ROOT/mempool_multi_node_bench"
CONTROL_CLI="$BUILD_ROOT/hogd/hog_exec/bench/control_cli"
COMMON_SSH_OPTS=("${CI_SSH_OPTS[@]}")
COMMON_SCP_OPTS=(-o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no)

. "$REPO_ROOT/scripts/hog-ip.sh"
. "$REPO_ROOT/scripts/git_info.sh"

SLOT_A_CPUS='0-31,64-95'
SLOT_B_CPUS='32-63,96-127'
HOG_EXEC_SLOT_A_PORT=8040
HOG_EXEC_SLOT_B_PORT=8041
HOG_EXEC_SLOT_A_HTTP_PORT=8042
HOG_EXEC_SLOT_B_HTTP_PORT=8043

ALL_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
NODE_IDS=(0 1)
CHUCKD_HOSTS=(hog4 hog8)
LOADGEN_HOSTS=(hog4 hog8)
AUTH_HOSTS=(hog4 hog8)
HOG_EXEC_HOSTS=(hog5 hog9)
EVENT_HOSTS=(hog6 hog10)
INDEXER_HOSTS=(hog6 hog10)
HOG_DB_HOSTS=(hog7 hog11)
MEMPOOL_HOSTS=(hog7 hog11)
NODE_HOST_GROUPS=("hog4 hog5 hog6 hog7" "hog8 hog9 hog10 hog11")

LOADGEN_BINARY="$BUILD_ROOT/hogd/loadgen/loadgen_server"
CHUCKD_BINARY="$BUILD_ROOT/chuckd.dev"
SCPD_BINARY="$BUILD_ROOT/scpd.dev"
MEMPOOL_BINARY="$BUILD_ROOT/hogd/hog_mempool/hog-mempool"
EVENT_BINARY="$BUILD_ROOT/hogd/hog_event/hog-event"
INDEXER_BINARY="$BUILD_ROOT/hogd/hog_indexer/hog-indexer"
HOG_EXEC_BINARY="$BUILD_ROOT/hogd/hog_exec/hog-exec"
HOG_DB_BINARY="$BUILD_ROOT/hogd/hog_db/hog-db"
AUTH_BINARY="$BUILD_ROOT/hogd/hog_auth/hog-auth"
CHUCKD_CONFIG_DIR="$REPO_ROOT/scripts/ci/chuckd-configs"
CHUCKD_INSTANCES=(970c9b 982951)
CHUCKD_SEEDS=(7704454085476916988 668639415096168636)

export CI_CONTROL_PORT=$("$BUILD_ROOT/hogd/utils/get_port" kCIControlServerPort)
export LOADGEN_PORT=11222
export LOADGEN_HTTP_PORT=11223
export AUTH_DB_PORT=8050
export AUTH_MEMPOOL_PORT=8051
export AUTH_HTTP_PORT=8052
export INDEXER_EVENT_PORT=8060
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

node_dir() {
  printf '%s/node%s' "$BENCH_ROOT" "$1"
}

node_log_dir() {
  printf '%s/logs' "$(node_dir "$1")"
}

node_storage_dir() {
  printf '%s/chuckd-storage' "$(node_dir "$1")"
}

quote_cmd() {
  local quoted=''
  printf -v quoted '%q ' "$@"
  printf '%s' "${quoted% }"
}

make_ulimit_exec_script() {
  local quoted_cmd
  quoted_cmd=$(quote_cmd "$@")
  cat <<EOF
set -euo pipefail
ulimit -c unlimited || true
exec ${quoted_cmd}
EOF
}

declare -a CONTROL_LOG_RECORDS=()
declare -a LIVE_TAIL_PIDS=()
PUBLISHED_RESULTS=0
COORDINATOR_OUTPUT_WRITTEN=0

record_control_logs() {
  local host=$1
  local label=$2
  local stdout_path=$3
  local stderr_path=$4
  CONTROL_LOG_RECORDS+=("${host}|${label}|${stdout_path}|${stderr_path}")
}

control_start_with_ulimit() {
  local host=$1
  local label=$2
  local cores=$3
  shift 3
  local script
  local output
  local stdout_path
  local stderr_path
  script=$(make_ulimit_exec_script "$@")
  if ! output=$("$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" start --cores "$cores" \
    /usr/bin/bash -lc "$script" 2>&1); then
    printf '%s\n' "$output"
    return 1
  fi
  printf '%s\n' "$output"
  stdout_path=$(printf '%s\n' "$output" | sed -n 's/^CONTROL_STDOUT_PATH=//p' | tail -n 1)
  stderr_path=$(printf '%s\n' "$output" | sed -n 's/^CONTROL_STDERR_PATH=//p' | tail -n 1)
  if [[ -z $stdout_path || -z $stderr_path ]]; then
    echo "failed to parse control cli log paths for $label on $host" >&2
    return 1
  fi
  record_control_logs "$host" "$label" "$stdout_path" "$stderr_path"
}

remote_run() {
  local host=$1
  shift
  ssh "${COMMON_SSH_OPTS[@]}" "$host" "$@"
}

copy_remote_file() {
  local host=$1
  local remote_path=$2
  local local_path=$3
  scp "${COMMON_SCP_OPTS[@]}" "${host}:${remote_path}" "$local_path"
}

publish_multi_node_logs() {
  local metrics_branch_dir
  local logs_dir
  local record
  local host
  local label
  local stdout_path
  local stderr_path

  metrics_branch_dir="$REPO_ROOT/metrics_history/metrics/nightly_integration_mempool_multi_node/$GIT_branch"
  logs_dir="$metrics_branch_dir/${GIT_commit_time}_$GIT_commit"_logs
  mkdir -p "$logs_dir"

  for record in "${CONTROL_LOG_RECORDS[@]}"; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    echo "copying ${label} logs from ${host}"
    copy_remote_file "$host" "$stdout_path" "$logs_dir/${host}_${label}.stdout.log"
    copy_remote_file "$host" "$stderr_path" "$logs_dir/${host}_${label}.stderr.log"
  done
}

get_record_stdout_path() {
  local record
  local host
  local label
  local stdout_path
  local stderr_path
  local wanted_label=$1

  for record in "${CONTROL_LOG_RECORDS[@]}"; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    if [[ $label == "$wanted_label" ]]; then
      printf '%s\n' "$stdout_path"
      return 0
    fi
  done

  echo "missing control log record for $wanted_label" >&2
  return 1
}

get_record_stderr_path() {
  local record
  local host
  local label
  local stdout_path
  local stderr_path
  local wanted_label=$1

  for record in "${CONTROL_LOG_RECORDS[@]}"; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    if [[ $label == "$wanted_label" ]]; then
      printf '%s\n' "$stderr_path"
      return 0
    fi
  done

  echo "missing control log record for $wanted_label" >&2
  return 1
}

start_log_stream() {
  local host=$1
  local label=$2
  local stream=$3
  local remote_path=$4
  local quoted_path
  local quoted_prefix

  printf -v quoted_path '%q' "$remote_path"
  printf -v quoted_prefix '%q' "[${host} ${label} ${stream}] "
  setsid /usr/bin/bash -lc \
    "ssh ${COMMON_SSH_OPTS[*]} $(printf '%q' "$host") \"tail -n +1 -F $quoted_path\" | sed \"s/^/${quoted_prefix}/\"" &
  LIVE_TAIL_PIDS+=("$!")
}

stop_live_log_streams() {
  local pid

  for pid in "${LIVE_TAIL_PIDS[@]}"; do
    kill -- "-$pid" >/dev/null 2>&1 || kill "$pid" >/dev/null 2>&1 || true
  done
  LIVE_TAIL_PIDS=()
}

write_coordinator_output() {
  local tmp_node0
  local tmp_node1
  local node0_stdout
  local node1_stdout

  if [[ $COORDINATOR_OUTPUT_WRITTEN -eq 1 ]]; then
    return 0
  fi

  node0_stdout=$(get_record_stdout_path node0_chuckd) || return 1
  node1_stdout=$(get_record_stdout_path node1_chuckd) || return 1

  tmp_node0=$(mktemp)
  tmp_node1=$(mktemp)
  copy_remote_file "${CHUCKD_HOSTS[0]}" "$node0_stdout" "$tmp_node0"
  copy_remote_file "${CHUCKD_HOSTS[1]}" "$node1_stdout" "$tmp_node1"
  echo "writing both outputs to $REPO_ROOT/coordinator_output.txt"
  cat "$tmp_node0" "$tmp_node1" >"$REPO_ROOT/coordinator_output.txt"
  rm -f "$tmp_node0" "$tmp_node1"
  COORDINATOR_OUTPUT_WRITTEN=1
}

publish_results() {
  if [[ $PUBLISHED_RESULTS -eq 1 ]]; then
    return 0
  fi

  if [[ ${#CONTROL_LOG_RECORDS[@]} -eq 0 ]]; then
    echo 'no control log records captured; skipping metrics_history publication'
    return 0
  fi

  echo 'clone metrics history # This is idempotent'
  "$REPO_ROOT/scripts/clone_metrics_history.sh"

  echo 'copy full remote logs into metrics history'
  publish_multi_node_logs

  if write_coordinator_output; then
    echo 'log integration metrics'
    "$REPO_ROOT/scripts/log_metric.sh" nightly_integration_mempool_multi_node "$REPO_ROOT/coordinator_output.txt"
  else
    echo 'failed to collect coordinator output; skipping summary metric file' >&2
  fi

  if [[ ${MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS:-0} -eq 0 ]]; then
    echo 'commit metrics history updates'
    "$REPO_ROOT/scripts/commit_metrics.sh"
  else
    echo 'skipping metrics history commit'
  fi
  PUBLISHED_RESULTS=1
}

kill_stale_processes() {
  ci_remote_killall_hosts "loadgen_server hog-exec hog-event hog-indexer hog-db hog-auth hog-mempool chuckd chuckd.dev scpd scpd.dev" "${ALL_HOSTS[@]}"
}

# Stops everything this suite launched, through the control servers that
# launched it. Every other benchmark script does this on the way out
# (exec_scaling.sh, mempool_bench.sh, metrics.sh, event_bench.sh); this one did
# not, and left [kill_stale_processes] as the only teardown it had.
#
# That is not a teardown these hosts actually have. The sweep signals with
# [sudo -n /usr/bin/killall], which every run on this fleet reports as
#
#   [hog4] cannot signal processes: sudo: a password is required
#   warning: could not reap processes on hog4 (status 3)
#
# on all eight hosts, so it reaps nothing. A run that ends without reaching the
# schedule's own shutdown -- killed at the job timeout, disconnected from a
# host, cancelled -- therefore leaves this suite's servers running on their
# fixed ports, and the next run binds those same ports: run #1869 died twice
# over on ports a previous run had left occupied, with
# "bind(10.100.0.105:8040): Address already in use" from both of node 0's
# hog-exec slots and the same on the mempool's 8075.
#
# The control server is the parent of what it started, so stopping through it
# needs no privilege and does not depend on the sweep being fixed. It is also
# synchronous: [ciserver_stop] waits for each child to exit before returning,
# so the ports are released by the time this returns rather than on the way to
# being released.
stop_launched_processes() {
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${ALL_HOSTS[@]}"
}

cleanup() {
  local exit_code=$1
  ci_enter_cleanup
  stop_live_log_streams
  # Results are published first: the logs it collects come off the hosts over
  # ssh and do not depend on the processes still running, but a stop that hangs
  # must not be able to cost us the diagnostics for the failure being cleaned
  # up after.
  publish_results
  stop_launched_processes
  kill_stale_processes
  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM

echo 'make sure bench servers are running'
# This gate waits on every host at once and collapses them to one status, so say
# which gate failed rather than exiting 1 with only the per-host lines above.
if ! ci_wait_port_hosts "$CI_CONTROL_PORT" "${ALL_HOSTS[@]}"; then
  echo "readiness: a bench control server is not listening on $CI_CONTROL_PORT" >&2
  exit 1
fi

echo 'clear work dirs'
for host in "${ALL_HOSTS[@]}"; do
  "$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" clear_work_dir
done

echo 'kill stale processes'
kill_stale_processes

echo 'clear benchmark work dirs'
for node in "${NODE_IDS[@]}"; do
  for host in ${NODE_HOST_GROUPS[$node]}; do
    remote_run "$host" "rm -rf $(printf '%q' "$(node_dir "$node")") && mkdir -p $(printf '%q' "$(node_log_dir "$node")")"
  done
done

echo 'launch loadgen servers'
for node in "${NODE_IDS[@]}"; do
  control_start_with_ulimit "${LOADGEN_HOSTS[$node]}" "node${node}_loadgen" "$SLOT_A_CPUS" \
    "$LOADGEN_BINARY" --bind-ip "$(hog_ip "${LOADGEN_HOSTS[$node]}")" --port "$LOADGEN_PORT" --metrics-port "$LOADGEN_HTTP_PORT"
done

echo 'wait for loadgen servers'
for node in "${NODE_IDS[@]}"; do
  "$WAIT_PORT" "$(hog_ip "${LOADGEN_HOSTS[$node]}")" "$LOADGEN_PORT"
  "$WAIT_PORT" "$(hog_ip "${LOADGEN_HOSTS[$node]}")" "$LOADGEN_HTTP_PORT"
done

echo 'launch hog-exec shards'
for node in "${NODE_IDS[@]}"; do
  coordinator_addr="$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_EXECUTOR_PORT"
  bind_ip="$(hog_ip "${HOG_EXEC_HOSTS[$node]}")"
  control_start_with_ulimit "${HOG_EXEC_HOSTS[$node]}" "node${node}_hog_exec_slot_a" "$SLOT_A_CPUS" \
    "$HOG_EXEC_BINARY" --coordinator-addr "$coordinator_addr" --bind-ip "$bind_ip" --nthreads 1024 --mempool-port "$HOG_EXEC_SLOT_A_PORT" --metrics-port "$HOG_EXEC_SLOT_A_HTTP_PORT"
  control_start_with_ulimit "${HOG_EXEC_HOSTS[$node]}" "node${node}_hog_exec_slot_b" "$SLOT_B_CPUS" \
    "$HOG_EXEC_BINARY" --coordinator-addr "$coordinator_addr" --bind-ip "$bind_ip" --nthreads 1024 --mempool-port "$HOG_EXEC_SLOT_B_PORT" --metrics-port "$HOG_EXEC_SLOT_B_HTTP_PORT"
done

echo 'launch event and indexer'
for node in "${NODE_IDS[@]}"; do
  event_host="${EVENT_HOSTS[$node]}"
  control_start_with_ulimit "$event_host" "node${node}_event" "$SLOT_A_CPUS" \
    "$EVENT_BINARY" --mempool-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_EVENT_PORT" --bind-ip "$(hog_ip "$event_host")" \
    --work-dir "$(node_dir "$node")/event" --executor-port "$EVENT_EXECUTOR_PORT" --mempool-port "$EVENT_MEMPOOL_PORT" --indexer-port "$EVENT_INDEXER_PORT" --metrics-port "$EVENT_HTTP_PORT"
  control_start_with_ulimit "${INDEXER_HOSTS[$node]}" "node${node}_indexer" "$SLOT_B_CPUS" \
    "$INDEXER_BINARY" --event-addr "$(hog_ip "$event_host"):$EVENT_INDEXER_PORT" --db-path "$(node_dir "$node")/indexer" \
    --bind-ip "$(hog_ip "${INDEXER_HOSTS[$node]}")" --event-port "$INDEXER_EVENT_PORT" --block-explorer-port "$INDEXER_BLOCK_EXPLORER_PORT" --metrics-port "$INDEXER_HTTP_PORT"
done

echo 'launch hog-db and auth'
for node in "${NODE_IDS[@]}"; do
  control_start_with_ulimit "${HOG_DB_HOSTS[$node]}" "node${node}_hog_db" "$SLOT_A_CPUS" \
    "$HOG_DB_BINARY" --db-path "$(node_dir "$node")/hog-db" --ram-bytes 10000000000 --bind-ip "$(hog_ip "${HOG_DB_HOSTS[$node]}")" \
    --coordinator-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_DB_PORT" --executor-port "$DB_EXECUTOR_PORT" \
    --mempool-port "$DB_MEMPOOL_PORT" --auth-port "$DB_AUTH_PORT" --metrics-port "$DB_HTTP_PORT"
  control_start_with_ulimit "${AUTH_HOSTS[$node]}" "node${node}_auth" "$SLOT_B_CPUS" \
    "$AUTH_BINARY" --mempool-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_AUTH_PORT" --bind-ip "$(hog_ip "${AUTH_HOSTS[$node]}")" \
    --db-port "$AUTH_DB_PORT" --mempool-port "$AUTH_MEMPOOL_PORT" --metrics-port "$AUTH_HTTP_PORT"
done

echo 'launch coordinators'
for node in "${NODE_IDS[@]}"; do
  control_start_with_ulimit "${CHUCKD_HOSTS[$node]}" "node${node}_chuckd" "$SLOT_A_CPUS" \
    "$CHUCKD_BINARY" -l info --skip 5 --stack --config "$CHUCKD_CONFIG_DIR/$node.json" --instance "${CHUCKD_INSTANCES[$node]}" --seed "${CHUCKD_SEEDS[$node]}" \
    --metrics :2223 bench coordinator_loadgen --in-addr "$(hog_ip "${CHUCKD_HOSTS[$node]}")":7888 --debug-port 5500 \
    --num-accts 10000 --txs-per-block 1000000 --num-blocks 1000 --storage-dir "$(node_storage_dir "$node")" \
    --out-addr :50000 --loadgen-addr "$(hog_ip "${LOADGEN_HOSTS[$node]}"):$LOADGEN_PORT" \
    --sched-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_COORDINATOR_PORT" --scpd-path "$SCPD_BINARY" \
    --scp-handle-addr 127.0.0.1:3300 --scp-control-addr 127.0.0.1:4400
done

echo 'stream coordinator stdout logs'
for node in "${NODE_IDS[@]}"; do
  start_log_stream "${CHUCKD_HOSTS[$node]}" "node${node}_chuckd" stdout \
    "$(get_record_stdout_path "node${node}_chuckd")"
done

echo 'launch mempools'
node0_mempool_host="${MEMPOOL_HOSTS[0]}"
node1_mempool_host="${MEMPOOL_HOSTS[1]}"
node0_mempool_cmd=(
  "$MEMPOOL_BINARY"
  --bind-ip "$(hog_ip "$node0_mempool_host")"
  --network-hash 0000000000000000000000000000000000000000000000000000000000000001
  --num-db-shards 1
  --num-exec-shards 2
  --db-port "$MEMPOOL_DB_PORT"
  --event-port "$MEMPOOL_EVENT_PORT"
  --executor-port "$MEMPOOL_EXECUTOR_PORT"
  --auth-port "$MEMPOOL_AUTH_PORT"
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
  --metrics-port "$MEMPOOL_HTTP_PORT"
)
node1_mempool_cmd=(
  "$MEMPOOL_BINARY"
  --bind-ip "$(hog_ip "$node1_mempool_host")"
  --network-hash 0000000000000000000000000000000000000000000000000000000000000001
  --num-db-shards 1
  --num-exec-shards 2
  --db-port "$MEMPOOL_DB_PORT"
  --event-port "$MEMPOOL_EVENT_PORT"
  --executor-port "$MEMPOOL_EXECUTOR_PORT"
  --auth-port "$MEMPOOL_AUTH_PORT"
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
  --metrics-port "$MEMPOOL_HTTP_PORT"
)

node0_mempool_script=$(
  cat <<EOF
set -euo pipefail
ulimit -c unlimited || true
set +e
$(quote_cmd timeout 360s "${node0_mempool_cmd[@]}")
status=\$?
set -e
[ \$status -eq 124 ] && status=0
exit \$status
EOF
)
node1_mempool_script=$(
  cat <<EOF
set -euo pipefail
ulimit -c unlimited || true
set +e
$(quote_cmd timeout 360s "${node1_mempool_cmd[@]}")
status=\$?
set -e
[ \$status -eq 124 ] && status=0
exit \$status
EOF
)

control_start_with_ulimit "$node0_mempool_host" "node0_mempool" "$SLOT_B_CPUS" \
  /usr/bin/bash -lc "$node0_mempool_script"
control_start_with_ulimit "$node1_mempool_host" "node1_mempool" "$SLOT_B_CPUS" \
  /usr/bin/bash -lc "$node1_mempool_script"

echo 'stream mempool stdout/stderr logs'
start_log_stream "$node0_mempool_host" node0_mempool stdout \
  "$(get_record_stdout_path node0_mempool)"
start_log_stream "$node0_mempool_host" node0_mempool stderr \
  "$(get_record_stderr_path node0_mempool)"
start_log_stream "$node1_mempool_host" node1_mempool stdout \
  "$(get_record_stdout_path node1_mempool)"
start_log_stream "$node1_mempool_host" node1_mempool stderr \
  "$(get_record_stderr_path node1_mempool)"

"$CONTROL_CLI" "$node0_mempool_host" "$CI_CONTROL_PORT" wait &
node0_mempool_pid=$!
"$CONTROL_CLI" "$node1_mempool_host" "$CI_CONTROL_PORT" wait &
node1_mempool_pid=$!

echo 'wait for process readiness'
for node in "${NODE_IDS[@]}"; do
  hog_exec_ip="$(hog_ip "${HOG_EXEC_HOSTS[$node]}")"
  "$WAIT_PORT" "$hog_exec_ip" "$HOG_EXEC_SLOT_A_PORT"
  "$WAIT_PORT" "$hog_exec_ip" "$HOG_EXEC_SLOT_B_PORT"
  "$WAIT_PORT" "$hog_exec_ip" "$HOG_EXEC_SLOT_A_HTTP_PORT"
  "$WAIT_PORT" "$hog_exec_ip" "$HOG_EXEC_SLOT_B_HTTP_PORT"

  event_ip="$(hog_ip "${EVENT_HOSTS[$node]}")"
  "$WAIT_PORT" "$event_ip" "$EVENT_EXECUTOR_PORT"
  "$WAIT_PORT" "$event_ip" "$EVENT_MEMPOOL_PORT"
  "$WAIT_PORT" "$event_ip" "$EVENT_INDEXER_PORT"
  "$WAIT_PORT" "$event_ip" "$EVENT_HTTP_PORT"

  indexer_ip="$(hog_ip "${INDEXER_HOSTS[$node]}")"
  "$WAIT_PORT" "$indexer_ip" "$INDEXER_EVENT_PORT"
  "$WAIT_PORT" "$indexer_ip" "$INDEXER_BLOCK_EXPLORER_PORT"
  "$WAIT_PORT" "$indexer_ip" "$INDEXER_HTTP_PORT"

  mempool_ip="$(hog_ip "${MEMPOOL_HOSTS[$node]}")"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_DB_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_EVENT_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_EXECUTOR_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_AUTH_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_COORDINATOR_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_HTTP_PORT"

  hog_db_ip="$(hog_ip "${HOG_DB_HOSTS[$node]}")"
  "$WAIT_PORT" "$hog_db_ip" "$DB_EXECUTOR_PORT"
  "$WAIT_PORT" "$hog_db_ip" "$DB_MEMPOOL_PORT"
  "$WAIT_PORT" "$hog_db_ip" "$DB_AUTH_PORT"
  "$WAIT_PORT" "$hog_db_ip" "$DB_HTTP_PORT"

  auth_ip="$(hog_ip "${AUTH_HOSTS[$node]}")"
  "$WAIT_PORT" "$auth_ip" "$AUTH_DB_PORT"
  "$WAIT_PORT" "$auth_ip" "$AUTH_MEMPOOL_PORT"
  "$WAIT_PORT" "$auth_ip" "$AUTH_HTTP_PORT"

  "$WAIT_PORT" "$(hog_ip "${CHUCKD_HOSTS[$node]}")" 7888
done

# A mempool that dies mid-run takes the suite down with it, and by that point
# every readiness wait has already passed, so nothing else in this script has
# anything to say about it. Name the status and replay the mempool's own log
# before exiting on it.
await_mempool() {
  local label=$1
  local pid=$2
  local status=0

  wait "$pid" || status=$?
  if [ "$status" -ne 0 ]; then
    echo "$label $(ci_describe_exit_status "$status")" >&2
    replay_control_log_for_label "$label"
    return "$status"
  fi
  echo "$label wait completed"
}

await_mempool node0_mempool "$node0_mempool_pid"
await_mempool node1_mempool "$node1_mempool_pid"

publish_results
