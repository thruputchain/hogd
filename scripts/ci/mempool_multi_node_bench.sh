#!/usr/bin/env bash
set -Eeuo pipefail

: "${BUILD_DIR:?BUILD_DIR must be set}"
: "${WORK_DIR:?WORK_DIR must be set}"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
WAIT_PORT="$REPO_ROOT/scripts/wait-port.sh"
BUILD_ROOT=$([[ $BUILD_DIR = /* ]] && printf '%s' "$BUILD_DIR" || printf '%s' "$REPO_ROOT/$BUILD_DIR")
WORK_ROOT=$([[ $WORK_DIR = /* ]] && printf '%s' "$WORK_DIR" || printf '%s' "$REPO_ROOT/$WORK_DIR")
BENCH_ROOT="$WORK_ROOT/mempool_multi_node_bench"
CONTROL_CLI="$BUILD_ROOT/hogd/hogexec/bench/control_cli"
COMMON_SSH_OPTS=(-n -T -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no)

. "$REPO_ROOT/scripts/hog-ip.sh"

SLOT_A_CPUS='0-31,64-95'
SLOT_B_CPUS='32-63,96-127'
HOGEXEC_SLOT_A_PORT=8040
HOGEXEC_SLOT_B_PORT=8041

ALL_HOSTS=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
NODE_IDS=(0 1)
CHUCKD_HOSTS=(hog4 hog8)
LOADGEN_HOSTS=(hog4 hog8)
AUTH_HOSTS=(hog4 hog8)
HOGEXEC_HOSTS=(hog5 hog9)
EVENT_HOSTS=(hog6 hog10)
INDEXER_HOSTS=(hog6 hog10)
HOGDB_HOSTS=(hog7 hog11)
MEMPOOL_HOSTS=(hog7 hog11)
NODE_HOST_GROUPS=("hog4 hog5 hog6 hog7" "hog8 hog9 hog10 hog11")

LOADGEN_BINARY="$BUILD_ROOT/hogd/loadgen/loadgen_server"
CHUCKD_BINARY="$BUILD_ROOT/chuckd.dev"
SCPD_BINARY="$BUILD_ROOT/scpd.dev"
MEMPOOL_BINARY="$BUILD_ROOT/hogd/mempool/mempool_exec"
EVENT_BINARY="$BUILD_ROOT/hogd/event/event_exec"
INDEXER_BINARY="$BUILD_ROOT/hogd/indexer/indexer_exec"
HOGEXEC_BINARY="$BUILD_ROOT/hogd/hogexec/hogexec"
HOGDB_BINARY="$BUILD_ROOT/hogd/hogdb/hogdb"
AUTH_BINARY="$BUILD_ROOT/hogd/auth/auth_exec"
CHUCKD_CONFIG_DIR="$REPO_ROOT/scripts/ci/chuckd-configs"
CHUCKD_INSTANCES=(970c9b 982951)
CHUCKD_SEEDS=(7704454085476916988 668639415096168636)

export CI_CONTROL_PORT=$("$BUILD_ROOT/hogd/utils/get_port" kCIControlServerPort)
export LOADGEN_PORT=11222
export AUTH_DB_PORT=8050
export AUTH_MEMPOOL_PORT=8051
export INDEXER_EVENT_PORT=8060
export INDEXER_BLOCK_EXPLORER_PORT=8061
export MEMPOOL_COORDINATOR_PORT=8070
export MEMPOOL_EXECUTOR_PORT=8071
export MEMPOOL_DB_PORT=8072
export MEMPOOL_EVENT_PORT=8073
export MEMPOOL_AUTH_PORT=8074
export DB_EXECUTOR_PORT=8080
export DB_MEMPOOL_PORT=8081
export DB_AUTH_PORT=8082
export EVENT_EXECUTOR_PORT=8090
export EVENT_MEMPOOL_PORT=8091
export EVENT_INDEXER_PORT=8092

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

control_start_with_ulimit() {
  local host=$1
  local cores=$2
  shift 2
  local script
  script=$(make_ulimit_exec_script "$@")
  "$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" start --cores "$cores" \
    /usr/bin/bash -lc "$script"
}

remote_run() {
  local host=$1
  shift
  ssh "${COMMON_SSH_OPTS[@]}" "$host" "$@"
}

latest_control_stdout() {
  local host=$1
  ssh "${COMMON_SSH_OPTS[@]}" "$host" 'ls -1t /tmp/hogd_stdout* | head -n 1'
}

kill_stale_processes() {
  local host
  for host in "${ALL_HOSTS[@]}"; do
    remote_run "$host" "killall -q loadgen_server hogexec event_exec indexer_exec hogdb auth_exec mempool_exec chuckd chuckd.dev scpd scpd.dev" || true
  done
}

cleanup() {
  local exit_code=$1
  trap - EXIT INT TERM
  kill_stale_processes
  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM

echo 'make sure bench servers are running'
for host in "${ALL_HOSTS[@]}"; do
  "$WAIT_PORT" "$host" "$CI_CONTROL_PORT"
done

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
  control_start_with_ulimit "${LOADGEN_HOSTS[$node]}" "$SLOT_A_CPUS" \
    "$LOADGEN_BINARY" --port "$LOADGEN_PORT"
done

echo 'wait for loadgen servers'
for node in "${NODE_IDS[@]}"; do
  "$WAIT_PORT" "$(hog_ip "${LOADGEN_HOSTS[$node]}")" "$LOADGEN_PORT"
done

echo 'launch hogexec shards'
for node in "${NODE_IDS[@]}"; do
  coordinator_addr="$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_EXECUTOR_PORT"
  bind_ip="$(hog_ip "${HOGEXEC_HOSTS[$node]}")"
  control_start_with_ulimit "${HOGEXEC_HOSTS[$node]}" "$SLOT_A_CPUS" \
    "$HOGEXEC_BINARY" --coordinator-addr "$coordinator_addr" --bind-ip "$bind_ip" --nthreads 1024 --mempool-port "$HOGEXEC_SLOT_A_PORT"
  control_start_with_ulimit "${HOGEXEC_HOSTS[$node]}" "$SLOT_B_CPUS" \
    "$HOGEXEC_BINARY" --coordinator-addr "$coordinator_addr" --bind-ip "$bind_ip" --nthreads 1024 --mempool-port "$HOGEXEC_SLOT_B_PORT"
done

echo 'launch event and indexer'
for node in "${NODE_IDS[@]}"; do
  event_host="${EVENT_HOSTS[$node]}"
  control_start_with_ulimit "$event_host" "$SLOT_A_CPUS" \
    "$EVENT_BINARY" --mempool-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_EVENT_PORT" --bind-ip "$(hog_ip "$event_host")" \
    --work-dir "$(node_dir "$node")/event" --executor-port "$EVENT_EXECUTOR_PORT" --mempool-port "$EVENT_MEMPOOL_PORT" --indexer-port "$EVENT_INDEXER_PORT"
  control_start_with_ulimit "${INDEXER_HOSTS[$node]}" "$SLOT_B_CPUS" \
    "$INDEXER_BINARY" --event-addr "$(hog_ip "$event_host"):$EVENT_INDEXER_PORT" --db-path "$(node_dir "$node")/indexer" \
    --bind-ip "$(hog_ip "${INDEXER_HOSTS[$node]}")" --event-port "$INDEXER_EVENT_PORT" --block-explorer-port "$INDEXER_BLOCK_EXPLORER_PORT"
done

echo 'launch hogdb and auth'
for node in "${NODE_IDS[@]}"; do
  control_start_with_ulimit "${HOGDB_HOSTS[$node]}" "$SLOT_A_CPUS" \
    "$HOGDB_BINARY" --db-path "$(node_dir "$node")/hogdb" --ram-bytes 10000000000 --bind-ip "$(hog_ip "${HOGDB_HOSTS[$node]}")" \
    --coordinator-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_DB_PORT" --executor-port "$DB_EXECUTOR_PORT" \
    --mempool-port "$DB_MEMPOOL_PORT" --auth-port "$DB_AUTH_PORT"
  control_start_with_ulimit "${AUTH_HOSTS[$node]}" "$SLOT_B_CPUS" \
    "$AUTH_BINARY" --mempool-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_AUTH_PORT" --bind-ip "$(hog_ip "${AUTH_HOSTS[$node]}")" \
    --db-port "$AUTH_DB_PORT" --mempool-port "$AUTH_MEMPOOL_PORT"
done

echo 'launch coordinators'
for node in "${NODE_IDS[@]}"; do
  control_start_with_ulimit "${CHUCKD_HOSTS[$node]}" "$SLOT_A_CPUS" \
    "$CHUCKD_BINARY" -l info --skip 5 --stack --config "$CHUCKD_CONFIG_DIR/$node.json" --instance "${CHUCKD_INSTANCES[$node]}" --seed "${CHUCKD_SEEDS[$node]}" \
    --metrics :2223 bench coordinator_loadgen --in-addr "$(hog_ip "${CHUCKD_HOSTS[$node]}")":7888 --debug-port 5500 \
    --num-accts 10000 --txs-per-block 1000000 --num-blocks 1000 --storage-dir "$(node_storage_dir "$node")" \
    --out-addr :50000 --wallet-addr :40000 --loadgen-addr "$(hog_ip "${LOADGEN_HOSTS[$node]}"):$LOADGEN_PORT" \
    --sched-addr "$(hog_ip "${MEMPOOL_HOSTS[$node]}"):$MEMPOOL_COORDINATOR_PORT" --scpd-path "$SCPD_BINARY" \
    --scp-handle-addr 127.0.0.1:3300 --scp-control-addr 127.0.0.1:4400
done

echo 'launch mempools'
node0_mempool_host="${MEMPOOL_HOSTS[0]}"
node1_mempool_host="${MEMPOOL_HOSTS[1]}"
node0_mempool_cmd=(
  "$MEMPOOL_BINARY"
  --bind-ip "$(hog_ip "$node0_mempool_host")"
  --network-id 1
  --num-db-shards 1
  --num-exec-shards 2
  --db-port "$MEMPOOL_DB_PORT"
  --event-port "$MEMPOOL_EVENT_PORT"
  --executor-port "$MEMPOOL_EXECUTOR_PORT"
  --auth-port "$MEMPOOL_AUTH_PORT"
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
)
node1_mempool_cmd=(
  "$MEMPOOL_BINARY"
  --bind-ip "$(hog_ip "$node1_mempool_host")"
  --network-id 1
  --num-db-shards 1
  --num-exec-shards 2
  --db-port "$MEMPOOL_DB_PORT"
  --event-port "$MEMPOOL_EVENT_PORT"
  --executor-port "$MEMPOOL_EXECUTOR_PORT"
  --auth-port "$MEMPOOL_AUTH_PORT"
  --coordinator-port "$MEMPOOL_COORDINATOR_PORT"
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

"$CONTROL_CLI" "$node0_mempool_host" "$CI_CONTROL_PORT" start --cores "$SLOT_B_CPUS" \
  /usr/bin/bash -lc "$node0_mempool_script"
"$CONTROL_CLI" "$node1_mempool_host" "$CI_CONTROL_PORT" start --cores "$SLOT_B_CPUS" \
  /usr/bin/bash -lc "$node1_mempool_script"

"$CONTROL_CLI" "$node0_mempool_host" "$CI_CONTROL_PORT" wait &
node0_mempool_pid=$!
"$CONTROL_CLI" "$node1_mempool_host" "$CI_CONTROL_PORT" wait &
node1_mempool_pid=$!

echo 'wait for process readiness'
for node in "${NODE_IDS[@]}"; do
  hogexec_ip="$(hog_ip "${HOGEXEC_HOSTS[$node]}")"
  "$WAIT_PORT" "$hogexec_ip" "$HOGEXEC_SLOT_A_PORT"
  "$WAIT_PORT" "$hogexec_ip" "$HOGEXEC_SLOT_B_PORT"

  event_ip="$(hog_ip "${EVENT_HOSTS[$node]}")"
  "$WAIT_PORT" "$event_ip" "$EVENT_EXECUTOR_PORT"
  "$WAIT_PORT" "$event_ip" "$EVENT_MEMPOOL_PORT"
  "$WAIT_PORT" "$event_ip" "$EVENT_INDEXER_PORT"

  indexer_ip="$(hog_ip "${INDEXER_HOSTS[$node]}")"
  "$WAIT_PORT" "$indexer_ip" "$INDEXER_EVENT_PORT"
  "$WAIT_PORT" "$indexer_ip" "$INDEXER_BLOCK_EXPLORER_PORT"

  mempool_ip="$(hog_ip "${MEMPOOL_HOSTS[$node]}")"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_DB_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_EVENT_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_EXECUTOR_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_AUTH_PORT"
  "$WAIT_PORT" "$mempool_ip" "$MEMPOOL_COORDINATOR_PORT"

  hogdb_ip="$(hog_ip "${HOGDB_HOSTS[$node]}")"
  "$WAIT_PORT" "$hogdb_ip" "$DB_EXECUTOR_PORT"
  "$WAIT_PORT" "$hogdb_ip" "$DB_MEMPOOL_PORT"
  "$WAIT_PORT" "$hogdb_ip" "$DB_AUTH_PORT"

  auth_ip="$(hog_ip "${AUTH_HOSTS[$node]}")"
  "$WAIT_PORT" "$auth_ip" "$AUTH_DB_PORT"
  "$WAIT_PORT" "$auth_ip" "$AUTH_MEMPOOL_PORT"

  "$WAIT_PORT" "$(hog_ip "${CHUCKD_HOSTS[$node]}")" 7888
done

wait "$node0_mempool_pid"
echo 'node0 mempool wait completed'
wait "$node1_mempool_pid"
echo 'node1 mempool wait completed'

echo 'collect coordinator output'
tmp_node0=$(mktemp)
tmp_node1=$(mktemp)
echo "copying coordinator output from ${CHUCKD_HOSTS[0]}"
scp -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no \
  "${CHUCKD_HOSTS[0]}:$(latest_control_stdout "${CHUCKD_HOSTS[0]}")" "$tmp_node0"
echo "copying coordinator output from ${CHUCKD_HOSTS[1]}"
scp -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no \
  "${CHUCKD_HOSTS[1]}:$(latest_control_stdout "${CHUCKD_HOSTS[1]}")" "$tmp_node1"
cat "$tmp_node0" "$tmp_node1" >"$REPO_ROOT/coordinator_output.txt"
rm -f "$tmp_node0" "$tmp_node1"

echo 'clone metrics history # This is idempotent'
"$REPO_ROOT/scripts/clone_metrics_history.sh"

echo 'log integration metrics'
"$REPO_ROOT/scripts/log_metric.sh" nightly_integration_mempool_multi_node "$REPO_ROOT/coordinator_output.txt"
