#!/bin/sh
# Build a WASM contract with controlled environment
# Usage: build_wasm.sh <target_name> <target_dir> [additional cargo flags...]

set -e

TARGET_NAME="$1"
TARGET_DIR="$2"

if [ -z "$TARGET_NAME" ] || [ -z "$TARGET_DIR" ]; then
    echo "Usage: build_wasm.sh <target_name> <target_dir> [additional cargo flags...]" >&2
    exit 1
fi

# Shift to get remaining args (if any)
shift 2

# Clean environment
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS

# Set environment variables
export CARGO_NET_GIT_FETCH_WITH_CLI=true

# Build using contract.toml config (includes rustflags, build-std, profile settings)
exec cargo --config .cargo/contract.toml build \
    -p "${TARGET_NAME}" \
    --locked \
    --frozen \
    --offline \
    --target-dir "${TARGET_DIR}" \
    "$@"
