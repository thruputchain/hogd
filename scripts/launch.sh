#!/bin/bash

CHUCKD_BINARY=~/go/bin/chuckd
LOADGEN_BINARY=/home/local/jhumphri/build/hogd/loadgen/loadgen_server
HOGDB_BINARY=/home/local/jhumphri/build/hogd/hogdb/hogdb
HOGEXEC_BINARY=/home/local/jhumphri/build/hogd/hogexec/hogexec

set -Eeuo pipefail

# Kill the whole process group on exit or when interrupted.
cleanup() {
  status=$?                                # remember why we’re exiting
  trap - EXIT INT TERM                     # prevent re-entry
  # Send SIGTERM to *entire* process group (children + grandchildren)
  kill -TERM -0 2>/dev/null || true        # no-op: touches PG, helps set $? on some shells
  kill -TERM -- -$$ 2>/dev/null || true    # polite stop
  # Give them a moment to exit gracefully
  sleep 1
  # Anything stubborn gets SIGKILL
  kill -KILL -- -$$ 2>/dev/null || true
  exit $status
}
trap cleanup EXIT INT TERM

if [ ! -x "$CHUCKD_BINARY" ]; then
  # Tell [go install] where the groundhog private repositories are.
  go env -w GOPRIVATE=github.com/groundhogchain/*
  # Tell [go install] to use SSH to access GitHub rather than HTTPS. This allows
  # it to use private key SSH authentication rather than requiring a private API
  # token from GitHub.
  git config --global url."ssh://git@github.com/".insteadOf "https://github.com/"
  # Install chuckd. This compiles the chuckd binary and places it at
  # [~/go/bin/chuckd].
  go install github.com/groundhogchain/chuckd/chuckd@latest
fi

wait_port() {
  local port=$1
  for i in {1..50}; do
    if ss -ltnp 2>/dev/null | grep -q ":$port "; then
      return 0
    fi
    sleep 0.2
  done
  echo "Timed out waiting for port $port to open" >&2
  return 1
}

# Start hogd loadgen. loadgen exits when either (1) a character is passed to
# stdin or (2) stdin is closed. If we just launch the loadgen process in the
# background with ["$LOADGEN_BINARY" -p 11222 &], stdin gets closed immediately,
# so the loadgen process immediately exits. To keep stdin open, we run the
# loadgen process in its own shell.
setsid -w "$LOADGEN_BINARY" -p 11222 < /dev/tty &
wait_port 11222

# Start chuckd.
"$CHUCKD_BINARY"                 \
  bench loadgen                  \
  --stack                        \
  --level info                   \
  --in-addr :7888                \
  --loadgen-addr localhost:11222 \
  --num-accts 10000              \
  --num-txs 1000000              \
  --exec-batch 100               \
  --exec-par 10                  \
  --max-msg-size 200000000 &
wait_port 7888

# Start hogdb.
echo "Start hogdb"
"$HOGDB_BINARY" /tmp/hogdb 1000000 127.0.0.1:7888 &
sleep 5

# Start hogexec.
echo "Start hogexec"
"$HOGEXEC_BINARY" 127.0.0.1:7888 1 &
sleep 5

# TODO(jhumphri): Launch the coordinator for the mempool.
# TODO(jhumphri): Launch the coordinator for the executor.
# TODO(jhumphri): Launch the mempool.
# TODO(jhumphri): Launch the executor.
# TODO(jhumphri): Launch the database that the mempool accesses.
# TODO(jhumphri): Launch the database that the executor accesses.

pids=($(jobs -pr))
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    # Causes EXIT trap to run, which kills the rest.
    exit 1
  fi
done
