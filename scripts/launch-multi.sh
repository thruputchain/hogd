#!/bin/bash

# -------------------------------
# These parameters are CONFIGURABLE.
CHUCKD_BUILD_DIR=~/chuckd/chuckd
HOGD_BUILD_DIR=/home/local/jhumphri/build

loadgen_server="hog3"
chuckd_server="hog3"
hogexec_servers=("hog4" "hog5")
hogdb_servers=("hog6")

# Set to 1 to enable x23 RPC logging.
export X23TRACE=0
# -------------------------------

# -------------------------------
# The parameters below here are NOT CONFIGURABLE.
CHUCKD_BINARY=${CHUCKD_BUILD_DIR}/chuckd
LOADGEN_BINARY=${HOGD_BUILD_DIR}/hogd/loadgen/loadgen_server
HOGEXEC_BINARY=${HOGD_BUILD_DIR}/hogd/hogexec/hogexec
HOGDB_BINARY=${HOGD_BUILD_DIR}/hogd/hogdb/hogdb

procs=("chuckd" "loadgen_server" "hogexec" "hogdb")

binaries=("$CHUCKD_BINARY" "$LOADGEN_BINARY" "$HOGEXEC_BINARY" "$HOGDB_BINARY")
all_servers=("$loadgen_server" "$chuckd_server" "${hogexec_servers[@]}" "${hogdb_servers[@]}")
# -------------------------------

# The code starts below.

# Kills all of the processes on all of the servers.
kill_everything() {
  killall -9 "${procs[@]}" >/dev/null 2>&1
  for s in "${all_servers[@]}"; do
    ssh "$s" "killall -9 ${procs[@]} > /dev/null 2>&1 || true"
  done
}

echo "Killing stale processes..."
kill_everything

set -Eeuo pipefail

# Kill all processes on all servers on exit or when interrupted.
cleanup() {
  status=$?

  kill_everything
  # Prevent re-entry into the trap handler.
  trap - EXIT INT TERM
  exit $status
}
trap cleanup EXIT INT TERM

build_all() {
  for s in "${all_servers[@]}"; do
    ssh "$s" "bash -lc 'go build -C \"$CHUCKD_BUILD_DIR\"'" | sed "s/^/[$s] /" &
    ssh "$s" "bash -lc 'make -C \"$HOGD_BUILD_DIR\" -j\$(nproc)'" | sed "s/^/[$s] /" &
  done
  wait
}
build_all

# Checks that the binaries are the same on all servers.
check_binaries() {
  for b in "${binaries[@]}"; do
    local reference_hash=""

    for s in "${all_servers[@]}"; do
      # Run ssh as the condition so set -e won't kill the script on failure
      if remote_output=$(ssh -o BatchMode=yes -o ConnectTimeout=5 \
        "$s" "sha256sum '${b}'" 2>/dev/null); then
        remote_hash=${remote_output%% *}
        if [ -z "$remote_hash" ]; then
          echo "$s: empty hash for file '${b}'."
          exit 1
        fi
        if [ -z "$reference_hash" ]; then
          reference_hash="$remote_hash"
        elif [ "$remote_hash" != "$reference_hash" ]; then
          echo "$s: hash mismatch for file '${b}'"
          exit 1
        fi
      else
        echo "$s: Missing file '${b}'."
        exit 1
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

# Start hogd loadgen. loadgen exits when either (1) a character is passed to
# stdin or (2) stdin is closed. If we just launch the loadgen process in the
# background with ["$LOADGEN_BINARY" -p 11222 &], stdin gets closed immediately,
# so the loadgen process immediately exits. To keep stdin open, we run the
# loadgen process in its own shell.
echo "Start loadgen..."
setsid -w "$LOADGEN_BINARY" -p 11222 </dev/tty >/tmp/loadgen.log &
wait_port localhost 11222

echo "Start chuckd..."
# Start chuckd.
ssh "$chuckd_server" "nohup \"$CHUCKD_BINARY\" \
  bench loadgen                                \
  --stack                                      \
  --level info                                 \
  --in-addr :7888                              \
  --loadgen-addr \"$loadgen_server\":11222     \
  --num-accts 10000                            \
  --num-txs 20000                              \
  --exec-batch 100                             \
  --exec-par 10                                \
  --max-msg-size 400000000                     \
  > /tmp/chuckd.log 2>&1 &"
wait_port "$chuckd_server" 7888

# Start hogexec.
echo "Start hogexec..."
for s in "${hogexec_servers[@]}"; do
  ssh "$s" "nohup \"$HOGEXEC_BINARY\" \"$chuckd_server\":7888 50 >/tmp/hogexec.log 2>&1 &"
done

# Start hogdb.
echo "Start hogdb..."
for s in "${hogdb_servers[@]}"; do
  # Remove any existing database so the experiment starts from a clean slate.
  ssh "$s" "rm -rf /tmp/hogdb; nohup \"$HOGDB_BINARY\" /tmp/hogdb 10000000000 \"$chuckd_server\":7888 50 > /tmp/hogdb.log 2>&1 &"
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

# Stream the logs from each server.
ssh "$loadgen_server" "tail -f /tmp/loadgen.log" | sed "s/^/[$loadgen_server] /" &
ssh "$chuckd_server" "tail -f /tmp/chuckd.log" | sed "s/^/[$chuckd_server] /" &

for s in "${hogexec_servers[@]}"; do
  ssh "$s" "tail -f /tmp/hogexec.log" | sed "s/^/[$s] /" &
done

for s in "${hogdb_servers[@]}"; do
  ssh "$s" "tail -f /tmp/hogdb.log" | sed "s/^/[$s] /" &
done

pids=($(jobs -pr))
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    # Causes EXIT trap to run, which kills the rest.
    exit 1
  fi
done
