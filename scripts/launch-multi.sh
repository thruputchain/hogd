#!/bin/bash

# These parameters are CONFIGURABLE.
# -------------------------------
HOGD_PROJECT_DIR=$(readlink -f $(dirname $0)/..)
HOGD_BUILD_DIR="/home/local/$(whoami)/build"

loadgen_server="hog3"
chuckd_server="hog3"
hogexec_servers=("hog4")
hogdb_servers=("hog4")
logdir=/tmp/$(whoami)

# override above in build or source directory hog.conf file
if test -r "$HOGD_BUILD_DIR/hog.conf"; then
  . "$HOGD_BUILD_DIR/hog.conf"
elif test -r "$HOGD_PROJECT_DIR/hog.conf"; then
  . "$HOGD_PROJECT_DIR/hog.conf"
fi

X23ENV=$(env | grep '^X23')
if test -n "$X23ENV"; then
    X23ENV="export $X23ENV"
else
    X23ENV=":"
fi

# -------------------------------

# The parameters below here are NOT CONFIGURABLE.
# -------------------------------
CHUCKD_BUILD_DIR=${HOGD_PROJECT_DIR}/groundhog-go/chuckd/chuckd
CHUCKD_BINARY=${HOGD_PROJECT_DIR}/groundhog-go/chuckd/chuckd/chuckd
LOADGEN_BINARY=${HOGD_BUILD_DIR}/hogd/loadgen/loadgen_server
HOGEXEC_BINARY=${HOGD_BUILD_DIR}/hogd/hogexec/hogexec
HOGDB_BINARY=${HOGD_BUILD_DIR}/hogd/hogdb/hogdb

procs=("chuckd" "loadgen_server" "hogexec" "hogdb")

binaries=("$CHUCKD_BINARY" "$LOADGEN_BINARY" "$HOGEXEC_BINARY" "$HOGDB_BINARY")
all_servers=("$loadgen_server" "$chuckd_server" "${hogexec_servers[@]}"
  "${hogdb_servers[@]}")

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
  local all_procs=("${procs[@]}" "make" "go")

  for s in "${all_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "killall -9 ${all_procs[@]} > /dev/null 2>&1 || true" &
  done
  if [[ $do_wait == true ]]; then
    wait
  fi
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
  kill -9 "${ssh_pids[@]}" 2>/dev/null

  # Now kill the chuckd and hogd processes on the servers. [kill_everything()]
  # issues the ssh commands as background processes, so these ssh commands will
  # continue to run after the script exits. I prefer to let the script exit
  # before the ssh commands complete so that it is not tempting to force quit
  # the script by hitting Ctrl-C multiple times. The processes will still get
  # killed on all of the servers.
  kill_everything false #do_wait=false

  exit $status
}
trap cleanup EXIT INT TERM

build_all() {
  for s in "${all_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "bash -lc 'go build -C \"$CHUCKD_BUILD_DIR\"'" | sed "s/^/[$s] /" &
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "bash -lc 'make -C \"$HOGD_BUILD_DIR\" -j\$(nproc)'" | sed "s/^/[$s] /" &
  done
  ssh_pids=($(jobs -pr))
  wait
}
build_all

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

echo "Checking binaries..."
check_binaries

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
ssh "${COMMON_SSH_OPTS[@]}" "$loadgen_server" \
  "$X23ENV; \
  nohup \"$LOADGEN_BINARY\" -p 11222 >$logdir/loadgen.log 2>&1 &"
wait_port "$loadgen_server" 11222

echo "Start chuckd..."
# Start chuckd.
ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" \
  "$X23ENV;                  \
  nohup \"$CHUCKD_BINARY\"                     \
  bench loadgen                                \
  --stack                                      \
  --level info                                 \
  --in-addr :7888                              \
  --loadgen-addr \"$loadgen_server\":11222     \
  --num-accts 10000                            \
  --num-txs 20000                              \
  --exec-batch 100                             \
  --exec-par 10                                \
  --num-blocks 100                             \
  --max-msg-size 400000000                     \
  > $logdir/chuckd.log 2>&1 &"
wait_port "$chuckd_server" 7888

# Start hogexec.
echo "Start hogexec..."
for s in "${hogexec_servers[@]}"; do
  ssh "${COMMON_SSH_OPTS[@]}" "$s" \
    "$X23ENV; \
    nohup \"$HOGEXEC_BINARY\" \"$chuckd_server\":7888 > $logdir/hogexec.log 2>&1 &"
done

# Start hogdb.
echo "Start hogdb..."
for s in "${hogdb_servers[@]}"; do
  # Remove any existing database so the experiment starts from a clean slate.
  ssh "${COMMON_SSH_OPTS[@]}" "$s" \
    "$X23ENV; \
    rm -rf $logdir/hogdb; \
    nohup \"$HOGDB_BINARY\" $logdir/hogdb 10000000000 \"$chuckd_server\":7888 \
    > $logdir/hogdb.log 2>&1 &"
done

# TODO(jhumphri): Launch the mempool.

# Wait for hogexec (port 8070).
for s in "${hogexec_servers[@]}"; do
  wait_port "$s" 8070
done

# Wait for hogdb (ports 8080, 8081, and 8082).
for s in "${hogdb_servers[@]}"; do
  wait_port "$s" 8080
  wait_port "$s" 8081
  wait_port "$s" 8082
done

echo "Done with launch."

if $results_only; then
  # Stream only the results.
  ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" \
    "tail -f $logdir/chuckd.log | grep --line-buffered obtain_delay_msec" |
    sed "s/^/[${chuckd_server}\/chuckd] /" &
else
  # Stream the logs from each server.
  ssh "${COMMON_SSH_OPTS[@]}" "$loadgen_server" \
    "tail -f $logdir/loadgen.log" | sed "s/^/[${loadgen_server}\/loadgen] /" &
  ssh "${COMMON_SSH_OPTS[@]}" "$chuckd_server" \
    "tail -f $logdir/chuckd.log" | sed "s/^/[${chuckd_server}\/chuckd] /" &

  for s in "${hogexec_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -f $logdir/hogexec.log" | sed "s/^/[${s}\/hogexec] /" &
  done

  for s in "${hogdb_servers[@]}"; do
    ssh "${COMMON_SSH_OPTS[@]}" "$s" \
      "tail -f $logdir/hogdb.log" | sed "s/^/[${s}\/hogdb] /" &
  done
fi

ssh_pids=($(jobs -pr))

# The script just waits forever until Ctrl-C is pressed.
tail -f /dev/null
