#!/bin/bash
# Idempotent install/update script for Cursor Cloud Agents.
# Runs from the repository root on each agent startup.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

echo "[cloud-agent] Initializing git submodules..."
# Cloud agents clone without submodules; private repos need org/repo access
# via Cursor secrets (see AGENTS.md).
git submodule update --init --recursive

echo "[cloud-agent] Writing scripts/conf.sh..."
cat > ./scripts/conf.sh <<'EOF'
GCC_CC=gcc-15
GCC_CXX=g++-15
CLANG_CC=/usr/bin/clang-21
CLANG_CXX=/usr/bin/clang++-21
CLANG_FORMAT=clang-format-21
EOF
chmod u+x ./scripts/conf.sh

echo "[cloud-agent] Fetching Rust crate dependencies..."
# Requires CARGO_REGISTRIES_GROUNDHOG_TOKEN in Cursor Secrets.
export CARGO_NET_GIT_FETCH_WITH_CLI="${CARGO_NET_GIT_FETCH_WITH_CLI:-true}"
export CARGO_REGISTRY_GLOBAL_CREDENTIAL_PROVIDERS="${CARGO_REGISTRY_GLOBAL_CREDENTIAL_PROVIDERS:-cargo:token}"
./scripts/fetch.sh

echo "[cloud-agent] Install complete."
