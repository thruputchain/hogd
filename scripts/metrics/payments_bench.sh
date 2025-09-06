#!/bin/bash

if [ $# -ne 2 ]; then
    echo "Usage: $0 <build_dir> <num_accounts>"
    exit 1
fi

BUILD_DIR=$1
NUM_ACCOUNTS=$2
CLIENT="$BUILD_DIR/hogd/hogexec/bench/payments_bench"

# Run the client process (waits for it to finish)
"$CLIENT" -c 10001 -e 10000 -a $NUM_ACCOUNTS -s hog11

