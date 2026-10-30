#!/bin/sh
# Run hogd's hog-devnode integration tests with hog-exec-hs as the executor.
#
# hog-devnode runs every hogd service in one process; the patch in this
# directory adds a test hook, HOGD_DEVNODE_EXTERNAL_EXECUTOR, that makes it
# start an external executor binary instead of its in-process one. The
# executor talks to the devnode's scheduler, hog_db and event server over the
# same RPC interfaces it uses in a real deployment.
#
# Usage: e2e/run-devnode-tests.sh BUILD_DIR [gtest args...]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BUILD_DIR=$(cd "${1:?usage: $0 BUILD_DIR [gtest args...]}" && pwd)
shift
cd "$ROOT"
if ! git apply --check -R "$HERE/devnode-external-executor.patch" 2>/dev/null; then
  git apply "$HERE/devnode-external-executor.patch"
fi
cmake --build "$BUILD_DIR" --target hog-devnode hog_devnode_test
(cd "$HERE/.." && cabal build -v0 exe:hog-exec-hs)
EXE=$(cd "$HERE/.." && cabal list-bin hog-exec-hs)
HOGD_DEVNODE_EXTERNAL_EXECUTOR="$EXE" \
  HOGD_DEVNODE_EXTERNAL_EXECUTOR_LOG="${HOGD_DEVNODE_EXTERNAL_EXECUTOR_LOG:-/dev/null}" \
  "$BUILD_DIR/hogd/devnode/hog_devnode_test" "$@"
