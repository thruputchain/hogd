#!/usr/bin/env bash

# Run before the C++ build, while the worker's installation token is fresh.
# Discover modules from the checked-out submodule rather than assuming its
# workspace layout. Keep workspace/local replacements active for each module.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
found=false
while IFS= read -r -d '' manifest; do
  found=true
  (cd "$(dirname "$manifest")" && go mod download)
done < <(find "$ROOT_DIR/thruput-go" -name .git -prune -o -name vendor -prune -o -name go.mod -print0)

if [[ "$found" != true ]]; then
  echo "No Go modules found in thruput-go; cannot prefetch chuckd/scpd dependencies" >&2
  exit 1
fi
