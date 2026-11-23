#!/bin/bash

if [ $# -ne 2 ]; then
  echo "Usage: $0 <build_dir> <num_accounts>"
  exit 1
fi

BUILD_DIR=$1
NUM_ACCOUNTS=$2
CLIENT="$BUILD_DIR/hogd/hog_exec/bench/payments_bench"

# Run the client process (waits for it to finish)
if [[ ${GIT_BRANCH:-} == codex/combined-transport-provenance ]]; then
  . "$(dirname "$0")/../ci/combined_transport_provenance.sh"
  combined_provenance_run "payments-$NUM_ACCOUNTS" "$CLIENT" -c 10001 -e 10000 -a $NUM_ACCOUNTS -s hog11 -s hog8
else
  "$CLIENT" -c 10001 -e 10000 -a $NUM_ACCOUNTS -s hog11 -s hog8
fi
