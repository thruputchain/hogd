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

rm -rf ~/.hogx
cp -R $HOGX_HOME/.hogx ~

# remove /tmp/hogx from prior runs
rm -rf /tmp/hogx/*
killall -9 indexer_exec auth_exec hogdb hogexec chuckd scpd mempool_exec event_exec loadgen_server

"$HOGX" config together -a loadgen -t 1
"$HOGX" start -a loadgen -c c1

echo "using: export HOGX_HOME=${HOGX_HOME}"
