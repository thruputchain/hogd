#!/bin/bash

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  echo "Usage: $0 <build_dir> <num_accounts> [rv32]"
  exit 1
fi

BUILD_DIR=$1
NUM_ACCOUNTS=$2
RUNTIME=${3:-wasm}
CLIENT="$BUILD_DIR/hogd/hogexec/bench/payments_bench"

# Optional third arg selects the contract runtime: "rv32" deploys the .rv32a
# artifacts, anything else (default) uses the wasm builds.
RV32_FLAG=""
if [ "$RUNTIME" = "rv32" ]; then
  RV32_FLAG="-r"
fi

# Run the client process (waits for it to finish)
"$CLIENT" -c 10001 -e 10000 -a $NUM_ACCOUNTS $RV32_FLAG -s hog11 -s hog8
