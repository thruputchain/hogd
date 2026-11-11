#!/usr/bin/env bash

# Run before the C++ build, while the worker's installation token is fresh.
# Discover modules from the checked-out submodule rather than assuming its
# workspace layout. Keep workspace/local replacements active for each module.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# `go mod download` records a checksum for every module in the graph in
# go.work.sum and go.sum, files thruput-go tracks. This is only a prefetch, so
# put them back afterwards: a modified submodule makes the build stamp
# "<commit>+dirty" into hogd's version, and control_server then rejects peers
# built from the same commit without running this script ("version mismatch").
# The downloaded modules stay in the module cache.
saved="$(mktemp -d)"
sums=()
while IFS= read -r -d '' sum; do
  cp -p -- "$sum" "$saved/${#sums[@]}"
  sums+=("$sum")
done < <(find "$ROOT_DIR/thruput-go" -name .git -prune -o -name vendor -prune -o \
  \( -name go.sum -o -name go.work.sum \) -type f -print0)
restore_sums() {
  local i
  for i in "${!sums[@]}"; do
    cp -p -- "$saved/$i" "${sums[$i]}"
  done
  rm -rf -- "$saved"
}
trap restore_sums EXIT

found=false
while IFS= read -r -d '' manifest; do
  found=true
  (cd "$(dirname "$manifest")" && go mod download)
done < <(find "$ROOT_DIR/thruput-go" -name .git -prune -o -name vendor -prune -o -name go.mod -print0)

if [[ "$found" != true ]]; then
  echo "No Go modules found in thruput-go; cannot prefetch chuckd/scpd dependencies" >&2
  exit 1
fi
