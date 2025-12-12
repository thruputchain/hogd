#!/bin/bash

# These parameters are CONFIGURABLE.
# -------------------------------
HOGD_PROJECT_DIR=$(readlink -f $(dirname $0)/..)
HOGD_BUILD_DIR="/home/hog12/$(whoami)/gcc"

loadgen_server="hog3"
chuckd_server="hog3"
mempool_server="hog3"
hogexec_servers=("hog4" "hog5" "hog2")
event_server="hog4"
hogdb_servers=("hog4")
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
LOADGEN_BINARY=${HOGD_BUILD_DIR}/hogd/loadgen/loadgen_server
CHUCKD_SCRIPT_DIR=${HOGD_PROJECT_DIR}/groundhog-go/chuckd/scripts
CHUCKD_BINARY=${HOGD_BUILD_DIR}/chuckd.dev
CHUCKD_CONFIG=$HOGD_PROJECT_DIR/scripts/config-chuckd.json
SCPD_BINARY=${HOGD_BUILD_DIR}/scpd.dev
MEMPOOL_BINARY=${HOGD_BUILD_DIR}/hogd/mempool/mempool_exec
EVENT_BINARY=${HOGD_BUILD_DIR}/hogd/event/event_exec
HOGEXEC_BINARY=${HOGD_BUILD_DIR}/hogd/hogexec/hogexec
HOGDB_BINARY=${HOGD_BUILD_DIR}/hogd/hogdb/hogdb

procs=("loadgen_server" "chuckd.dev" "scpd.dev" "mempool_exec" "hogexec" "event_exec" "hogdb")

binaries=("$LOADGEN_BINARY" "$CHUCKD_BINARY" "$SCPD_BINARY" "$MEMPOOL_BINARY" "$HOGEXEC_BINARY" "$EVENT_BINARY" "$HOGDB_BINARY")
all_servers=("$loadgen_server" "$chuckd_server" "$mempool_server" "${hogexec_servers[@]}" "$event_server" "${hogdb_servers[@]}")

COMMON_SSH_OPTS=(-n -T -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no)

results_only=false

for arg in "$@"; do
  if [[ $arg == "--results-only" ]]; then
    results_only=true
    break
  fi
done
# -------------------------------

# The code starts below.

. "$(dirname $0)/remote-exec.sh"

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

# Wait for the port to open on the remote machine.
wait_port() {
  local host=$1
  local port=$2
  while true; do
    if nc -z "$host" "$port" 2>/dev/null; then
      return 0
    fi
    sleep 0.2
  done
}

echo "Start loadgen..."
remote_launch "$loadgen_server" "$LOADGEN_BINARY" -p 11222
wait_port "$loadgen_server" 11222

# Start hogexec.
echo "Start hogexec..."
for s in "${hogexec_servers[@]}"; do
  remote_launch "$s" "$HOGEXEC_BINARY" "$mempool_server":8071 -n 1024
done

# Start event.
echo "Start event..."
remote_launch "$event_server" "$EVENT_BINARY" \
  "$mempool_server":8073 "$logdir/event"

# Start hogdb.
echo "Start hogdb..."
for s in "${hogdb_servers[@]}"; do
  # Remove any existing database so the experiment starts from a clean slate.
  ssh "${COMMON_SSH_OPTS[@]}" "$s" "rm -rf $logdir/hogdb.data"
  remote_launch "$s" "$HOGDB_BINARY" \
    "$logdir/hogdb.data" 10000000000 "$mempool_server":8072
done

# Start mempool.
echo "Start mempool..."
remote_launch "$mempool_server" "$MEMPOOL_BINARY" \
  ${#hogdb_servers[@]} ${#hogexec_servers[@]}
sleep 1

# Launch chuckd.
echo "Start chuckd..."
ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" "rm -rf \"$logdir\"/chuckd-storage"

remote_launch "$chuckd_server" "$CHUCKD_BINARY" --copy-file "$CHUCKD_CONFIG" \
  --copy-file "$SCPD_BINARY" \
  -l info --stack --config "$logdir"/config-chuckd.json \
  --instance 970c9b \
  --seed 1410441218572558875 \
  --metrics :2223 \
  bench coordinator_loadgen \
  --in-addr hog3:7888 \
  --debug-port 5500 \
  --num-accts 100 \
  --txs-per-block 10000 \
  --num-blocks 1000 \
  --storage-dir "$logdir"/chuckd-storage \
  --out-addr :50000 \
  --wallet-addr :40000 \
  --loadgen-addr "$loadgen_server":11222 \
  --sched-addr "$mempool_server":8070 \
  --scpd-path "$logdir"/scpd.dev \
  --scp-handle-addr 127.0.0.1:3300 \
  --scp-control-addr 127.0.0.1:4400 \
  --scp-is-validator true

# Wait for hogexec (port 8090).
for s in "${hogexec_servers[@]}"; do
  wait_port "$s" 8090
done

# Wait for event (ports 8091-8092).
wait_port "$event_server" 8091
wait_port "$event_server" 8092

# Wait for hogdb (ports 8080-8081).
for s in "${hogdb_servers[@]}"; do
  wait_port "$s" 8080
  wait_port "$s" 8081
done

# Wait for chuckd (port 7888).
wait_port hog3 7888

# Wait for mempool (ports 8070-8073).
wait_port "$mempool_server" 8070
wait_port "$mempool_server" 8071
wait_port "$mempool_server" 8072
wait_port "$mempool_server" 8073

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

  for s in "${hogdb_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -n +1 -f $logdir/hogdb.log" | sed "s/^/[${s}\/hogdb] /" &
  done
fi

ssh_pids=($(jobs -pr))

# The script just waits forever until Ctrl-C is pressed.
sleep infinity
