#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

CLIENT="$BUILD_DIR/hogd/hogexec/bench/fibonacci_wasmonly"
read -r -a HOGEXEC_SERVERS <<<"${METRICS_HOGEXEC_SERVERS:-hog11 hog8}"
HOGEXEC_SERVER_ARGS=()
for server in "${HOGEXEC_SERVERS[@]}"; do
  HOGEXEC_SERVER_ARGS+=(-s "$server")
done

# Run the client process (waits for it to finish)
"$CLIENT" -c 10001 -e 10000 "${HOGEXEC_SERVER_ARGS[@]}"
