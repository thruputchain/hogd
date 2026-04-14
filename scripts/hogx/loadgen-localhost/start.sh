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
killall -9 indexer_exec auth_exec hogdb hogexec chuckd scpd mempool_exec event_exec loadgen_server

"$HOGX" config together -a loadgen -t 1

# [$HOGX config together] generates additional config files in
# [$HOGX_HOME/.hogx]. Thus, we wait until after running that command to copy
# [.hogx] to the home directory. Additionally, due to these additional config
# files that are generated, we have to copy from [$HOGX_HOME] rather than from
# [$SCRIPT_DIR].
rm -rf ~/.hogx
cp -R $HOGX_HOME/.hogx ~

"$HOGX" start -a loadgen -c c1

echo "using: export HOGX_HOME=${HOGX_HOME}"
