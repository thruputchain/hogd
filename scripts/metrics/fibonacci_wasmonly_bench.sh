#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 <build_dir>"
    exit 1
fi

BUILD_DIR=$1

BIN="$BUILD_DIR/hogd/hogexec/bench/fibonacci_wasmonly"

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

"$BIN" -s < "$PIPE" > /dev/null 2>&1 &
S_PID=$!

# give it a moment to start
while ! nc -z localhost 10001; do
    sleep 0.1
done

# Run the -c process (waits for it to finish)
"$BIN" -c

# After -c finishes, send a character to the -s process to stop it
echo "q" >&3
