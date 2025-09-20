#!/bin/bash

BUILD_DIR=/home/local/jhumphri/build

CHUCKD_BINARY=~/go/bin/chuckd
LOADGEN_BINARY=${BUILD_DIR}/hogd/loadgen/loadgen_server
HOGEXEC_BINARY=${BUILD_DIR}/hogd/hogexec/hogexec
HOGDB_BINARY=${BUILD_DIR}/hogd/hogdb/hogdb

# Set to 1 to enable x23 RPC logging.
X23TRACE=0

killall -9 chuckd > /dev/null 2>&1
killall -9 loadgen_server > /dev/null 2>&1
killall -9 hogexec > /dev/null 2>&1
killall -9 hogdb > /dev/null 2>&1

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
setsid -w "$LOADGEN_BINARY" -p 11222 < /dev/tty &
wait_port localhost 11222

# Start chuckd.
"$CHUCKD_BINARY"                 \
  bench loadgen                  \
  --stack                        \
  --level info                   \
  --in-addr :7888                \
  --loadgen-addr localhost:11222 \
  --num-accts 10000              \
  --num-txs 20000                \
  --exec-batch 100               \
  --exec-par 10                  \
  --max-msg-size 400000000       \
  &
wait_port localhost 7888

# Start hogexec.
echo "Start hogexec"
# "$HOGEXEC_BINARY" 127.0.0.1:7888 50 &

sleep 10

# Start hogdb.
echo "Start hogdb"
# Remove any existing database so the experiment starts from a clean slate.
rm -rf /tmp/hogdb
"$HOGDB_BINARY" /tmp/hogdb 10000000000 127.0.0.1:7888 &

# TODO(jhumphri): Launch the mempool.

# Wait for hogexec (port 8070).
wait_port hog4 8070

# Wait for hogdb (ports 8080, 8081, and 8082).
wait_port localhost 8080
wait_port localhost 8081
wait_port localhost 8082

echo "Done with launch."

pids=($(jobs -pr))
for pid in "${pids[@]}"; do
  if ! wait "$pid"; then
    # Causes EXIT trap to run, which kills the rest.
    exit 1
  fi
done
