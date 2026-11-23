#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

CLIENT="$BUILD_DIR/hogd/hog_exec/bench/fibonacci_rv32only"

# Run the client process (waits for it to finish)
if [[ ${GIT_BRANCH:-} == codex/combined-transport-provenance ]]; then
  . "$(dirname "$0")/../ci/combined_transport_provenance.sh"
  combined_provenance_run "fibonacci" "$CLIENT" -c 10001 -e 10000 -s hog11 -s hog8
else
  "$CLIENT" -c 10001 -e 10000 -s hog11 -s hog8
fi
