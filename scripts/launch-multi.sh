#!/bin/bash

# These parameters are CONFIGURABLE.
# -------------------------------
HOGD_PROJECT_DIR=$(readlink -f $(dirname $0)/..)
HOGD_BUILD_DIR="/home/hog12/$(whoami)/gcc"

# loadgen_server="hog3"
# chuckd_server="hog3"
# mempool_server="hog3"
# hog_exec_servers=("hog1" "hog2")
# event_server="hog1"
# indexer_server="hog1"
# hog_db_servers=("hog1")
# auth_server="hog1"

# CI configuration.
loadgen_server="hog7"
chuckd_server="hog4"
mempool_server="hog10"
hog_exec_servers=("hog5" "hog6")
event_server="hog9"
indexer_server="hog9"
hog_db_servers=("hog11")
auth_server="hog8"
logdir=/tmp/$(whoami)
# Number of wallet accounts the load generator creates. This is the main
# lever on how much state the chain holds, and so on how much of the
# ramdb cache is actually used.
num_accts=10000
# Size of the synthetic recipient address space. When nonzero, work transfers
# pay one of these instead of another account, and nothing is deployed at them:
# the first payment to each inserts a new db item. That makes the state grow
# with the tx count, up to this many extra items, rather than staying at
# [num_accts], which is the other lever on the working set. Zero keeps every
# transfer inside the account set.
num_virtual_accts=0
# Per-shard ramdb cache. Total in-memory capacity is this times the shard
# count, so it decides whether a working set fits in memory or spills.
ram_bytes=10000000000
# Benchmark hack: synthetic state written straight into each shard's ramdb at
# startup, so a large cache can be measured full without first committing the
# transactions needed to fill it (~10 h per run at 300 GB). Each shard writes
# only the addresses it owns, so [preload_items] is a whole-db count: at 4 KiB
# an item, 72,000,000 fills one 300 GB shard, and 288,000,000 fills four.
# Zero disables it, which is the normal path.
# State-filler knobs, passed to the load generator. The filler makes state grow
# with the workload instead of with the transaction count: every
# [filler_period]'th work transaction calls a storage_filler contract that writes
# [filler_items] items of [filler_item_bytes] each. Many instances so the state
# spreads over shards rather than pinning to the one that owns a single contract
# address. Zero instances disables it.
filler_instances=0
filler_items=16
filler_item_bytes=1024
filler_period=1
# Distinct key groups the filler cycles through; 0 = always-fresh keys.
filler_keyspace=0
preload_items=0
preload_bytes_per_item=4096
# The workload chuckd generates. Bigger batches with fewer of them raise the
# offered-load ceiling, because chuckd waits at least a second between
# submissions regardless of batch size.
txs_per_block=100000
num_blocks=1000
# The hogdb data directory. A long run writes >100 GB here, so it should not be
# on a tmpfs.
hog_db_data_dir=/var/tmp/$(whoami)/hogdb.data

# override above in build or source directory hog.conf file
if test -r "$HOGD_BUILD_DIR/hog.conf"; then
  . "$HOGD_BUILD_DIR/hog.conf"
elif test -r "$HOGD_PROJECT_DIR/hog.conf"; then
  . "$HOGD_PROJECT_DIR/hog.conf"
fi

export MEMPOOL_DAG_DOT_DIR="$logdir/mempool_dot"
EXPORT_REGEX='^(X23|MEMPOOL_DAG_DOT_DIR)='

# -------------------------------

# The parameters below here are NOT CONFIGURABLE.
# -------------------------------
export LOADGEN_PORT=11222
export LOADGEN_HTTP_PORT=11223

export EXECUTOR_MEMPOOL_PORT=8640
export EXECUTOR_HTTP_PORT=8641

export AUTH_DB_PORT=8550
export AUTH_MEMPOOL_PORT=8551
export AUTH_HTTP_PORT=8552

export INDEXER_EVENT_PORT=8560
export INDEXER_BLOCK_EXPLORER_PORT=8561
export INDEXER_HTTP_PORT=8562

export MEMPOOL_COORDINATOR_PORT=8570
export MEMPOOL_EXECUTOR_PORT=8571
export MEMPOOL_DB_PORT=8572
export MEMPOOL_EVENT_PORT=8573
export MEMPOOL_AUTH_PORT=8574
export MEMPOOL_HTTP_PORT=8575

export DB_EXECUTOR_PORT=8580
export DB_MEMPOOL_PORT=8581
export DB_AUTH_PORT=8582
export DB_HTTP_PORT=8583

export EVENT_EXECUTOR_PORT=8590
export EVENT_MEMPOOL_PORT=8591
export EVENT_INDEXER_PORT=8592
export EVENT_HTTP_PORT=8593

LOADGEN_BINARY=${HOGD_BUILD_DIR}/hogd/loadgen/loadgen_server
CHUCKD_SCRIPT_DIR=${HOGD_PROJECT_DIR}/groundhog-go/chuckd/scripts
CHUCKD_BINARY=${HOGD_BUILD_DIR}/chuckd.dev
CHUCKD_CONFIG=$HOGD_PROJECT_DIR/scripts/config-chuckd.json
SCPD_BINARY=${HOGD_BUILD_DIR}/scpd.dev
MEMPOOL_BINARY=${HOGD_BUILD_DIR}/hogd/hog_mempool/hog-mempool
EVENT_BINARY=${HOGD_BUILD_DIR}/hogd/hog_event/hog-event
INDEXER_BINARY=${HOGD_BUILD_DIR}/hogd/hog_indexer/hog-indexer
HOG_EXEC_BINARY=${HOGD_BUILD_DIR}/hogd/hog_exec/hog-exec
HOG_DB_BINARY=${HOGD_BUILD_DIR}/hogd/hog_db/hog-db
AUTH_BINARY=${HOGD_BUILD_DIR}/hogd/hog_auth/hog-auth

procs=("loadgen_server" "chuckd.dev" "scpd.dev" "hog-mempool" "hog-exec" "hog-event" "hog-indexer" "hog-db" "hog-auth")

binaries=("$LOADGEN_BINARY" "$CHUCKD_BINARY" "$SCPD_BINARY" "$MEMPOOL_BINARY" "$HOG_EXEC_BINARY" "$EVENT_BINARY" "$INDEXER_BINARY" "$HOG_DB_BINARY")
all_servers=("$loadgen_server" "$chuckd_server" "$mempool_server" "${hog_exec_servers[@]}" "$event_server" "$indexer_server" "${hog_db_servers[@]}" "${auth_server}")

COMMON_SSH_OPTS=(-n -T -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no)

results_only=false
delete_db=true

for arg in "$@"; do
  case "$arg" in
  --results-only)
    results_only=true
    ;;
  --no-delete-db)
    delete_db=false
    ;;
  esac
done
# -------------------------------

# The code starts below.

. "$(dirname $0)/remote-exec.sh"
. "$(dirname $0)/hog-ip.sh"

remote_launch() {
  local ARGS=("$@") prog="$(basename $2)"
  ARGS+=(">$logdir/$prog.log" '2>&1')
  remote_exec "${ARGS[@]}" &
}

# This removes duplicates from [all_servers].
all_servers=($(printf "%s\n" "${all_servers[@]}" | awk '!seen[$0]++'))

make_logdir() {
  for s in "${all_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" mkdir -p "$logdir"
  done
  ssh "${COMMON_SSH_OPTS[@]}" "$mempool_server" mkdir -p "$MEMPOOL_DAG_DOT_DIR"
}

# Kills all of the processes on all of the servers.
kill_everything() {
  local do_wait=$1
  local all_procs=("${procs[@]}" "go")

  for s in "${all_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" "killall -9 -q -e ${all_procs[@]}" || :
  done
}

echo "Making log directories..."
make_logdir

echo "Killing stale processes..."
kill_everything true #do_wait=true

set -Eeuo pipefail

# Kill all processes on all servers on exit or when interrupted.
ssh_pids=()
cleanup() {
  status=$?

  # Prevent re-entry into the trap handler.
  trap - EXIT INT TERM

  # Kill the ssh sessions that print build output and log statements from the
  # servers.
  if [[ ${#ssh_pids[@]} != 0 ]]; then
    kill -9 "${ssh_pids[@]}" 2>/dev/null
  fi

  # Now kill the chuckd and hogd processes on the servers. [kill_everything()]
  # issues the ssh commands as background processes, so these ssh commands will
  # continue to run after the script exits. I prefer to let the script exit
  # before the ssh commands complete so that it is not tempting to force quit
  # the script by hitting Ctrl-C multiple times. The processes will still get
  # killed on all of the servers.
  kill_everything false & #do_wait=false

  exit $status
}
trap cleanup EXIT INT TERM

# Build only the binaries this script launches. Building everything also
# builds x23's `bench` target, which does not compile against crc32c's
# vendored Google Benchmark headers, and `set -e` would abort the run.
cmake --build "$HOGD_BUILD_DIR" --parallel "$(nproc)" --target \
  hog-mempool hog-db hog-exec hog-event hog-indexer hog-auth loadgen_server
# These build scripts place the binaries in the current working directory, so we
# [pushd "$HOG_BUILD_DIR"] so that the binaries are placed in the build
# directory.
pushd "$HOGD_BUILD_DIR" >/dev/null
"$CHUCKD_SCRIPT_DIR"/build_chuckd.sh
"$CHUCKD_SCRIPT_DIR"/build_scpd.sh
popd >/dev/null

# Checks that the binaries are the same on all servers.
check_binaries() {
  declare -a lines
  mapfile -t lines < <(
    for b in "${binaries[@]}"; do
      for s in "${all_servers[@]}"; do
        {
          if remote_output=$(ssh "${COMMON_SSH_OPTS[@]}" "$s" \
            "sha256sum -- '${b}'" 2>/dev/null); then
            remote_hash=${remote_output%% *}
            [[ -n $remote_hash ]] &&
              echo "$s $b $remote_hash" || echo "$s ERROR"
          else
            echo "$s ERROR"
          fi
        } &
      done
    done
    wait
  )

  # Parse results into a map with [binary-server] as the key.
  declare -A hashes=()
  for line in "${lines[@]}"; do
    read -r s b val <<<"$line"
    if [[ $val == "ERROR" ]]; then
      echo "$s: Missing file or empty hash for '$b'."
      return 1
    fi
    hashes["$b-$s"]="$val"
  done
  for key in "${!hashes[@]}"; do
    echo "$key => ${hashes[$key]}"
  done

  for b in "${binaries[@]}"; do
    local reference_hash=""

    for s in "${all_servers[@]}"; do
      remote_hash="${hashes["$b-$s"]}"
      if [[ -z $remote_hash ]]; then
        echo "$s: No result for '$b'."
        return 1
      fi
      if [[ -z $reference_hash ]]; then
        reference_hash="$remote_hash"
      elif [[ $remote_hash != "$reference_hash" ]]; then
        echo "$s: hash mismatch for '$b' \
          (got $remote_hash, expected $reference_hash)."
        return 1
      fi
    done
  done
}

#echo "Checking binaries..."
#check_binaries

echo "Start loadgen..."
remote_launch "$loadgen_server" "$LOADGEN_BINARY" --bind-ip "$(hog_ip "$loadgen_server")" --port $LOADGEN_PORT --metrics-port $LOADGEN_HTTP_PORT --num-virtual-accts $num_virtual_accts --filler-instances $filler_instances --filler-items $filler_items --filler-item-bytes $filler_item_bytes --filler-period $filler_period --filler-keyspace $filler_keyspace
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$loadgen_server")" $LOADGEN_PORT

# Start hog-exec.
echo "Start hog-exec..."
for s in "${hog_exec_servers[@]}"; do
  remote_launch "$s" "$HOG_EXEC_BINARY" --coordinator-addr "$(hog_ip "$mempool_server")":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip "$s")" --nthreads 1024 --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
done

# Start event.
echo "Start event..."
# The event work dir is never truncated by the event server, so without this it
# accumulates across runs -- ~20 GB each -- until $logdir's tmpfs runs out and
# the event server starts failing mid-run.
if $delete_db; then
  ssh "${COMMON_SSH_OPTS[@]}" "$event_server" "rm -rf $logdir/event"
fi
remote_launch "$event_server" "$EVENT_BINARY" \
  --mempool-addr "$(hog_ip "$mempool_server")":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip "$event_server")" --work-dir "$logdir/event" --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT --metrics-port $EVENT_HTTP_PORT

# Start indexer.
echo "Start indexer..."
if $delete_db; then
  ssh "${COMMON_SSH_OPTS[@]}" "$indexer_server" "rm -rf $logdir/indexer"
fi
remote_launch "$indexer_server" "$INDEXER_BINARY" \
  --event-addr "$(hog_ip "$event_server")":$EVENT_INDEXER_PORT --db-path "$logdir/indexer" --bind-ip "$(hog_ip "$indexer_server")" --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT --metrics-port $INDEXER_HTTP_PORT

# Start hog-db.
echo "Start hog-db..."
for i in "${!hog_db_servers[@]}"; do
  s="${hog_db_servers[$i]}"
  # Remove any existing database so the experiment starts from a clean slate.
  if $delete_db; then
    ssh "${COMMON_SSH_OPTS[@]}" "$s" "rm -rf $hog_db_data_dir"
  fi
  remote_launch "$s" "$HOG_DB_BINARY" \
    --db-path "$hog_db_data_dir" --ram-bytes $ram_bytes --bind-ip "$(hog_ip "$s")" --coordinator-addr "$(hog_ip "$mempool_server")":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT --metrics-port $DB_HTTP_PORT --shard-id "$i" --total-shards "${#hog_db_servers[@]}" --preload-items $preload_items --preload-bytes-per-item $preload_bytes_per_item
done

# Start auth.
echo "Start auth..."
remote_launch "$auth_server" "$AUTH_BINARY" \
  --mempool-addr "$(hog_ip "$mempool_server")":$MEMPOOL_AUTH_PORT --bind-ip "$(hog_ip "$auth_server")" --db-port $AUTH_DB_PORT --mempool-port $AUTH_MEMPOOL_PORT --metrics-port $AUTH_HTTP_PORT

# Start mempool.
echo "Start mempool..."
remote_launch "$mempool_server" "$MEMPOOL_BINARY" \
  --bind-ip "$(hog_ip "$mempool_server")" \
  --metrics-port $MEMPOOL_HTTP_PORT \
  --network-hash 0000000000000000000000000000000000000000000000000000000000000000 \
  --num-db-shards ${#hog_db_servers[@]} --num-exec-shards ${#hog_exec_servers[@]} \
  --db-port $MEMPOOL_DB_PORT --event-port $MEMPOOL_EVENT_PORT --executor-port $MEMPOOL_EXECUTOR_PORT --auth-port $MEMPOOL_AUTH_PORT --coordinator-port $MEMPOOL_COORDINATOR_PORT

# Wait for loadgen.
# We already waited on [$LOADGEN_PORT] above, so we only still need to wait on
# [$LOADGEN_HTTP_PORT].
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$loadgen_server")" $LOADGEN_HTTP_PORT

# Wait for hog-exec.
for s in "${hog_exec_servers[@]}"; do
  "$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$s")" $EXECUTOR_MEMPOOL_PORT
  "$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$s")" $EXECUTOR_HTTP_PORT
done

# Wait for event.
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$event_server")" $EVENT_EXECUTOR_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$event_server")" $EVENT_MEMPOOL_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$event_server")" $EVENT_INDEXER_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$event_server")" $EVENT_HTTP_PORT

# Wait for indexer.
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$indexer_server")" $INDEXER_BLOCK_EXPLORER_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$indexer_server")" $INDEXER_HTTP_PORT

# Wait for hog-db. A preloading shard fills its ramdb before it opens any of
# these ports, and that takes minutes at 300 GB -- the default 60 s timeout
# would abort the run mid-preload -- so allow for the bytes being written, at a
# deliberately pessimistic 300 MB/s.
db_wait_timeout=60
if [[ $preload_items -gt 0 ]]; then
  # Per shard, since [preload_items] is a whole-db count and each shard writes
  # only the addresses it owns. Measured fill rate starts near 0.5 GB/s and
  # decays past 0.2 GB/s as the ramdb grows, so budget at 120 MB/s and add slack
  # for scanning the addresses this shard does not own.
  db_wait_timeout=$((300 + preload_items * preload_bytes_per_item /
    ${#hog_db_servers[@]} / 60000000))
  # Everything downstream waits on the preload too: chuckd only opens its port
  # once the mempool is ready, and the mempool is ready only once genesis has
  # hashed the preloaded db. wait-port.sh takes this env var as its default
  # timeout, so one export covers chuckd and every mempool port. Without it the
  # 60 s default kills the run minutes after the preload finally succeeded.
  export CI_WAIT_PORT_TIMEOUT_SEC=$db_wait_timeout
fi
for s in "${hog_db_servers[@]}"; do
  "$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$s")" $DB_EXECUTOR_PORT $db_wait_timeout
  "$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$s")" $DB_MEMPOOL_PORT $db_wait_timeout
  "$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$s")" $DB_AUTH_PORT $db_wait_timeout
  "$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$s")" $DB_HTTP_PORT $db_wait_timeout
done

# Wait for auth.
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$auth_server")" $AUTH_DB_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$auth_server")" $AUTH_MEMPOOL_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$auth_server")" $AUTH_HTTP_PORT

# Launch chuckd here rather than before the databases, because chuckd cannot
# survive a long wait. It registers with the mempool, and the mempool is only
# ready once every db shard has registered -- which, when a shard preloads its
# ramdb, is minutes later. Waiting that long killed chuckd's idle RPC
# transport to scpd, and the first call after the wait
# (SCPSetKnownQuorumSet) then panicked through must.NoError, taking the whole
# run down at the moment the cluster finally came up.
# Launch chuckd.
echo "Start chuckd..."
if $delete_db; then
  ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" "rm -rf \"$logdir\"/chuckd-storage"
fi

remote_launch "$chuckd_server" "$CHUCKD_BINARY" --copy-file "$CHUCKD_CONFIG" \
  --copy-file "$SCPD_BINARY" \
  -l debug --stack --config "$logdir"/config-chuckd.json \
  --rpc-log-stats \
  --instance 970c9b \
  --seed 1410441218572558875 \
  --metrics :2323 \
  bench coordinator_loadgen \
  --in-addr "$(hog_ip "$chuckd_server")":7988 \
  --debug-port 5600 \
  --num-accts $num_accts \
  --txs-per-block $txs_per_block \
  --num-blocks $num_blocks \
  --storage-dir "$logdir"/chuckd-storage \
  --out-addr :50100 \
  --loadgen-addr "$(hog_ip "$loadgen_server")":$LOADGEN_PORT \
  --sched-addr "$(hog_ip "$mempool_server")":$MEMPOOL_COORDINATOR_PORT \
  --scpd-path "$logdir"/scpd.dev \
  --scp-handle-addr 127.0.0.1:3350 \
  --scp-control-addr 127.0.0.1:4450 \
  --scp-is-validator true

# Wait for chuckd.
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$chuckd_server")" 7988

# Wait for mempool.
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$mempool_server")" $MEMPOOL_COORDINATOR_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$mempool_server")" $MEMPOOL_EXECUTOR_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$mempool_server")" $MEMPOOL_DB_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$mempool_server")" $MEMPOOL_EVENT_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$mempool_server")" $MEMPOOL_AUTH_PORT
"$HOGD_PROJECT_DIR/scripts/wait-port.sh" "$(hog_ip "$mempool_server")" $MEMPOOL_HTTP_PORT

echo "Done with launch."

if $results_only; then
  # Stream only the results.
  ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" \
    "tail -n +1 -f $logdir/chuckd.log | grep --line-buffered obtain_delay_msec" |
    sed "s/^/[${chuckd_server}\/chuckd] /" &
else
  # Stream the logs from each server.
  ssh "${COMMON_SSH_OPTS[@]}" "$loadgen_server" \
    "tail -n +1 -f $logdir/loadgen.log" | sed "s/^/[${loadgen_server}\/loadgen] /" &
  ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" \
    "tail -n +1 -f $logdir/chuckd.dev.log" | sed "s/^/[${chuckd_server}\/chuckd] /" &
  ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" \
    "tail -n +1 -f $logdir/scpd.dev.log" |
    sed "s/^/[${chuckd_server}\/scpd] /" &

  ssh "${COMMON_SSH_OPTS[@]}" "$mempool_server" \
    "tail -n +1 -f $logdir/hog-mempool.log" | sed "s/^/[${mempool_server}\/mempool] /" &

  for s in "${hog_exec_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -n +1 -f $logdir/hog-exec.log" | sed "s/^/[${s}\/hog-exec] /" &
  done

  ssh "${COMMON_SSH_OPTS[@]}" "$event_server" \
    "tail -n +1 -f $logdir/hog-event.log" | sed "s/^/[${event_server}\/event] /" &

  ssh "${COMMON_SSH_OPTS[@]}" "$indexer_server" \
    "tail -n +1 -f $logdir/hog-indexer.log" | sed "s/^/[${indexer_server}\/indexer] /" &

  for s in "${hog_db_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -n +1 -f $logdir/hog-db.log" | sed "s/^/[${s}\/hog-db] /" &
  done

  ssh "${COMMON_SSH_OPTS[@]}" "$auth_server" \
    "tail -n +1 -f $logdir/hog-auth.log" | sed "s/^/[${auth_server}\/auth] /" &
fi

ssh_pids=($(jobs -pr))

# The script just waits forever until Ctrl-C is pressed.
sleep infinity
