#!/bin/sh

set -ex

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
if [ -z "${HOGX:-}" ]; then
  echo "HOGX environment variable is not set (expected path to hogx binary)" >&2
  exit 1
fi

export HOGX_HOME=/tmp/.hogx-loadgen-localhost
rm -rf $HOGX_HOME

cp -R $SCRIPT_DIR/.hogx $HOGX_HOME

# remove /tmp/hogx from prior runs
rm -rf /tmp/hogx/*

"$HOGX" config together -a loadgen -t 1 \
  --num-accounts 100 \
  --txs-per-block 100000 \
  --num-blocks 100

set +e
"$HOGX" run -a loadgen -c c1
hogx_exit_code=$?
set -e

# Ensure leftover agents from this host are cleaned up.
pkill -f hogx-agent || true

exit "$hogx_exit_code"
