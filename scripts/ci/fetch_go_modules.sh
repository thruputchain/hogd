#!/usr/bin/env bash

# Run before the C++ build, while the worker's installation token is fresh.
# Prefetch exactly what building and testing thruput-go's workspace needs:
# `work` is every package in the modules go.work uses, so this follows the
# submodule's layout rather than assuming it, and keeps its replacements.
#
# Not `go mod download`: with no arguments it fetches the whole module graph
# and records a checksum for every module in it in thruput-go's tracked
# go.work.sum. That leaves the submodule modified, the build then stamps
# "<commit>+dirty" into hogd's version, and control_server rejects peers built
# from the same commit without this prefetch ("version mismatch"). Listing the
# packages downloads and verifies the same modules the build will, and writes
# nothing while thruput-go's committed sums cover them.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ ! -f "$ROOT_DIR/thruput-go/go.work" ]]; then
  echo "No go.work in thruput-go; cannot prefetch chuckd dependencies" >&2
  exit 1
fi

go -C "$ROOT_DIR/thruput-go" list -deps -test work >/dev/null
