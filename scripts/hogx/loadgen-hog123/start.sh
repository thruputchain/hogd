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

# remove /tmp/hogx from prior runs on all hosts
for host in hog1 hog2 hog3; do
  ssh $host "rm -rf /tmp/hogx/*"
  ssh $host "killall -9 hog-indexer hog-auth hog-db hog-exec chuckd scpd hog-mempool hog-event loadgen_server || true"
done

"$HOGX" -m 3 config together -a loadgen -t 2

rm -rf ~/.hogx
# [$HOGX config together] generates additional config files in
# [$HOGX_HOME/.hogx]. Thus, we wait until after running that command to copy
# [.hogx] to the home directory. Additionally, due to these additional config
# files that are generated, we have to copy from [$HOGX_HOME] rather than from
# [$SCRIPT_DIR].
cp -R $HOGX_HOME/ ~/.hogx

"$HOGX" -m 3 start -a loadgen --network-hash 0000000000000000000000000000000000000000000000000000000000000001

echo "using: export HOGX_HOME=${HOGX_HOME}"
