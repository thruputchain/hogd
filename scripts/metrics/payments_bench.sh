#!/bin/bash

if [ $# -ne 2 ]; then
    echo "Usage: $0 <build_dir> <num_accounts>"
    exit 1
fi

BUILD_DIR=$1
NUM_ACCOUNTS=$2
CLIENT="$BUILD_DIR/hogd/hogexec/bench/payments_bench"
SERVER="$BUILD_DIR/hogd/hogexec/bench/bench_server"

PIPE=$(mktemp -u)
mkfifo "$PIPE"
exec 3<> "$PIPE"

cleanup() {
    if [[ -n "$S_PID" ]] && kill -0 "$S_PID" 2>/dev/null; then
        kill "$S_PID"
        wait "$S_PID"
    fi
    exec 3>&-  # Close the pipe if open
    rm -f "$PIPE"
}
trap cleanup EXIT INT TERM

"$SERVER" -c 10001 -e 10000 < "$PIPE" > /dev/null 2>&1 &
S_PID=$!

# give it a moment to start
while ! nc -z localhost 10001; do
    sleep 0.1
done
# Run the client process (waits for it to finish)
"$CLIENT" -c 10001 -e 10000 -a $NUM_ACCOUNTS

# After client finishes, send a character to the server process to stop it
echo "q" >&3
