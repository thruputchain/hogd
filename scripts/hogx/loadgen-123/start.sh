#!/bin/sh

set -ex

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
if [ -z "${HOGX:-}" ]; then
  echo "HOGX environment variable is not set (expected path to hogx binary)" >&2
  exit 1
fi

export HOGX_HOME=/tmp/.hogx-loadgen-123

cp -R $SCRIPT_DIR/.hogx $HOGX_HOME

"$HOGX" -m 3 config together
"$HOGX" -m 3 start
