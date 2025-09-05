#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 <build_dir>"
    exit 1
fi

BUILD_DIR=$1

CLIENT="$BUILD_DIR/hogd/hogexec/bench/fibonacci_wasmonly"

# Run the client process (waits for it to finish)
"$CLIENT" -c 10001 -e 10000 -s hog11

