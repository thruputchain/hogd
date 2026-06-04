#!/bin/bash

# These parameters are CONFIGURABLE.
# -------------------------------
HOGD_PROJECT_DIR=$(readlink -f $(dirname $0)/..)
HOGD_BUILD_DIR="/home/hog12/$(whoami)/gcc"

# loadgen_server="hog3"
# chuckd_server="hog3"
# mempool_server="hog3"
# hogexec_servers=("hog1" "hog2")
# event_server="hog1"
# indexer_server="hog1"
# hogdb_servers=("hog1")
# auth_server="hog1"

# CI configuration.
loadgen_server="hog7"
chuckd_server="hog4"
mempool_server="hog10"
hogexec_servers=("hog5" "hog6")
event_server="hog9"
indexer_server="hog9"
hogdb_servers=("hog11")
auth_server="hog8"
logdir=/tmp/$(whoami)

# override above in build or source directory hog.conf file
if test -r "$HOGD_BUILD_DIR/hog.conf"; then
  . "$HOGD_BUILD_DIR/hog.conf"
elif test -r "$HOGD_PROJECT_DIR/hog.conf"; then
  . "$HOGD_PROJECT_DIR/hog.conf"
fi

EXPORT_REGEX='^X23'

# -------------------------------

# The parameters below here are NOT CONFIGURABLE.
# -------------------------------
export LOADGEN_PORT=11222
export LOADGEN_HTTP_PORT=11223

export EXECUTOR_MEMPOOL_PORT=8040
export EXECUTOR_HTTP_PORT=8041

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

LOADGEN_BINARY=${HOGD_BUILD_DIR}/hogd/loadgen/loadgen_server
CHUCKD_SCRIPT_DIR=${HOGD_PROJECT_DIR}/groundhog-go/chuckd/scripts
CHUCKD_BINARY=${HOGD_BUILD_DIR}/chuckd.dev
CHUCKD_CONFIG=$HOGD_PROJECT_DIR/scripts/config-chuckd.json
SCPD_BINARY=${HOGD_BUILD_DIR}/scpd.dev
MEMPOOL_BINARY=${HOGD_BUILD_DIR}/hogd/mempool/mempool_exec
EVENT_BINARY=${HOGD_BUILD_DIR}/hogd/event/event_exec
INDEXER_BINARY=${HOGD_BUILD_DIR}/hogd/indexer/indexer_exec
HOGEXEC_BINARY=${HOGD_BUILD_DIR}/hogd/hogexec/hogexec
HOGDB_BINARY=${HOGD_BUILD_DIR}/hogd/hogdb/hogdb
AUTH_BINARY=${HOGD_BUILD_DIR}/hogd/auth/auth_exec

procs=("loadgen_server" "chuckd.dev" "scpd.dev" "mempool_exec" "hogexec" "event_exec" "indexer_exec" "hogdb" "auth_exec")

binaries=("$LOADGEN_BINARY" "$CHUCKD_BINARY" "$SCPD_BINARY" "$MEMPOOL_BINARY" "$HOGEXEC_BINARY" "$EVENT_BINARY" "$INDEXER_BINARY" "$HOGDB_BINARY")
all_servers=("$loadgen_server" "$chuckd_server" "$mempool_server" "${hogexec_servers[@]}" "$event_server" "$indexer_server" "${hogdb_servers[@]}" "${auth_server}")

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

  # Kill the ssh sessions that print the [make] output and log statements from
  # the servers.
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

make -C "$HOGD_BUILD_DIR" -j$(nproc)
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
remote_launch "$loadgen_server" "$LOADGEN_BINARY" --bind-ip "$(hog_ip "$loadgen_server")" --port $LOADGEN_PORT --metrics-port $LOADGEN_HTTP_PORT
./scripts/wait-port.sh "$(hog_ip "$loadgen_server")" $LOADGEN_PORT

# Start hogexec.
echo "Start hogexec..."
for s in "${hogexec_servers[@]}"; do
  remote_launch "$s" "$HOGEXEC_BINARY" --coordinator-addr "$(hog_ip "$mempool_server")":$MEMPOOL_EXECUTOR_PORT --bind-ip "$(hog_ip "$s")" --nthreads 1024 --mempool-port $EXECUTOR_MEMPOOL_PORT --metrics-port $EXECUTOR_HTTP_PORT
done

# Start event.
echo "Start event..."
remote_launch "$event_server" "$EVENT_BINARY" \
  --mempool-addr "$(hog_ip "$mempool_server")":$MEMPOOL_EVENT_PORT --bind-ip "$(hog_ip "$event_server")" --work-dir "$logdir/event" --executor-port $EVENT_EXECUTOR_PORT --mempool-port $EVENT_MEMPOOL_PORT --indexer-port $EVENT_INDEXER_PORT --metrics-port $EVENT_HTTP_PORT

# Start indexer.
echo "Start indexer..."
if $delete_db; then
  ssh "${COMMON_SSH_OPTS[@]}" "$indexer_server" "rm -rf $logdir/indexer"
fi
remote_launch "$indexer_server" "$INDEXER_BINARY" \
  --event-addr "$(hog_ip "$event_server")":$EVENT_INDEXER_PORT --db-path "$logdir/indexer" --bind-ip "$(hog_ip "$indexer_server")" --event-port $INDEXER_EVENT_PORT --block-explorer-port $INDEXER_BLOCK_EXPLORER_PORT --metrics-port $INDEXER_HTTP_PORT

# Start hogdb.
echo "Start hogdb..."
for s in "${hogdb_servers[@]}"; do
  # Remove any existing database so the experiment starts from a clean slate.
  if $delete_db; then
    ssh "${COMMON_SSH_OPTS[@]}" "$s" "rm -rf $logdir/hogdb.data"
  fi
  remote_launch "$s" "$HOGDB_BINARY" \
    --db-path "$logdir/hogdb.data" --ram-bytes 10000000000 --bind-ip "$(hog_ip "$s")" --coordinator-addr "$(hog_ip "$mempool_server")":$MEMPOOL_DB_PORT --executor-port $DB_EXECUTOR_PORT --mempool-port $DB_MEMPOOL_PORT --auth-port $DB_AUTH_PORT --metrics-port $HOGDB_HTTP_PORT
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
  --network-id 0 \
  --num-db-shards ${#hogdb_servers[@]} --num-exec-shards ${#hogexec_servers[@]} \
  --db-port $MEMPOOL_DB_PORT --event-port $MEMPOOL_EVENT_PORT --executor-port $MEMPOOL_EXECUTOR_PORT --auth-port $MEMPOOL_AUTH_PORT --coordinator-port $MEMPOOL_COORDINATOR_PORT

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
  --metrics :2223 \
  bench coordinator_loadgen \
  --in-addr "$(hog_ip "$chuckd_server")":7888 \
  --debug-port 5500 \
  --num-accts 10000 \
  --txs-per-block 1000000 \
  --num-blocks 1000 \
  --storage-dir "$logdir"/chuckd-storage \
  --out-addr :50000 \
  --loadgen-addr "$(hog_ip "$loadgen_server")":$LOADGEN_PORT \
  --sched-addr "$(hog_ip "$mempool_server")":$MEMPOOL_COORDINATOR_PORT \
  --scpd-path "$logdir"/scpd.dev \
  --scp-handle-addr 127.0.0.1:3300 \
  --scp-control-addr 127.0.0.1:4400 \
  --scp-is-validator true

# Wait for hogexec (port 8040).
for s in "${hogexec_servers[@]}"; do
  ./scripts/wait-port.sh "$(hog_ip "$s")" 8040
done

# Wait for event (ports 8090-8092).
./scripts/wait-port.sh "$(hog_ip "$event_server")" $EVENT_EXECUTOR_PORT
./scripts/wait-port.sh "$(hog_ip "$event_server")" $EVENT_MEMPOOL_PORT
./scripts/wait-port.sh "$(hog_ip "$event_server")" $EVENT_INDEXER_PORT
./scripts/wait-port.sh "$(hog_ip "$event_server")" $EVENT_HTTP_PORT

# Wait for indexer (ports 8060-8061).
./scripts/wait-port.sh "$(hog_ip "$indexer_server")" $INDEXER_EVENT_PORT
./scripts/wait-port.sh "$(hog_ip "$indexer_server")" $INDEXER_BLOCK_EXPLORER_PORT
./scripts/wait-port.sh "$(hog_ip "$indexer_server")" $INDEXER_HTTP_PORT

# Wait for hogdb (ports 8080-8082).
for s in "${hogdb_servers[@]}"; do
  ./scripts/wait-port.sh "$(hog_ip "$s")" $DB_EXECUTOR_PORT
  ./scripts/wait-port.sh "$(hog_ip "$s")" $DB_MEMPOOL_PORT
  ./scripts/wait-port.sh "$(hog_ip "$s")" $DB_AUTH_PORT
  ./scripts/wait-port.sh "$(hog_ip "$s")" $DB_HTTP_PORT
done

# Wait for auth (ports 8050-8051).
./scripts/wait-port.sh "$(hog_ip "$auth_server")" $AUTH_DB_PORT
./scripts/wait-port.sh "$(hog_ip "$auth_server")" $AUTH_MEMPOOL_PORT
./scripts/wait-port.sh "$(hog_ip "$auth_server")" $AUTH_HTTP_PORT

# Wait for chuckd (port 7888).
./scripts/wait-port.sh "$(hog_ip "$chuckd_server")" 7888

# Wait for mempool (ports 8070-8073).
./scripts/wait-port.sh "$(hog_ip "$mempool_server")" $MEMPOOL_COORDINATOR_PORT
./scripts/wait-port.sh "$(hog_ip "$mempool_server")" $MEMPOOL_EXECUTOR_PORT
./scripts/wait-port.sh "$(hog_ip "$mempool_server")" $MEMPOOL_DB_PORT
./scripts/wait-port.sh "$(hog_ip "$mempool_server")" $MEMPOOL_EVENT_PORT
./scripts/wait-port.sh "$(hog_ip "$mempool_server")" $MEMPOOL_AUTH_PORT
./scripts/wait-port.sh "$(hog_ip "$mempool_server")" $MEMPOOL_HTTP_PORT

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
    "tail -n +1 -f $logdir/mempool_exec.log" | sed "s/^/[${mempool_server}\/mempool] /" &

  for s in "${hogexec_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -n +1 -f $logdir/hogexec.log" | sed "s/^/[${s}\/hogexec] /" &
  done

  ssh "${COMMON_SSH_OPTS[@]}" "$event_server" \
    "tail -n +1 -f $logdir/event_exec.log" | sed "s/^/[${event_server}\/event] /" &

  ssh "${COMMON_SSH_OPTS[@]}" "$indexer_server" \
    "tail -n +1 -f $logdir/indexer_exec.log" | sed "s/^/[${indexer_server}\/indexer] /" &

  for s in "${hogdb_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -n +1 -f $logdir/hogdb.log" | sed "s/^/[${s}\/hogdb] /" &
  done

  ssh "${COMMON_SSH_OPTS[@]}" "$auth_server" \
    "tail -n +1 -f $logdir/auth_exec.log" | sed "s/^/[${auth_server}\/auth] /" &
fi

ssh_pids=($(jobs -pr))

# The script just waits forever until Ctrl-C is pressed.
sleep infinity
