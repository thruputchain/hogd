#!/bin/sh

set -ex

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
if [ -z "${HOGX:-}" ]; then
  echo "HOGX environment variable is not set (expected path to hogx binary)" >&2
  exit 1
fi

export HOGX_HOME=/tmp/.hogx-loadgen-hog123
rm -rf $HOGX_HOME

cp -R $SCRIPT_DIR/.hogx $HOGX_HOME
rm -rf ~/.hogx
cp -R $SCRIPT_DIR/.hogx ~

# remove /tmp/hogx from prior runs on all hosts
for host in hog1 hog2 hog3; do
  ssh $host "rm -rf /tmp/hogx/*"
  ssh $host "killall -9 indexer_exec auth_exec hogdb hogexec chuckd scpd mempool_exec event_exec loadgen_server || true"
done

"$HOGX" -m 3 config together -a loadgen -t 2
"$HOGX" -m 3 start -a loadgen --network-id 1

echo "using: export HOGX_HOME=${HOGX_HOME}"
