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
export RUSTFLAGS='-C link-args=-zstack-size=30000 -Zunstable-options -Cpanic=immediate-abort'

# Build the command with any additional flags from $@
exec cargo build \
    -p "${TARGET_NAME}" \
    --target wasm32-unknown-unknown \
    --release \
    --locked \
    --frozen \
    -F guest \
    --offline \
    --target-dir "${TARGET_DIR}" \
    -Zbuild-std=core,alloc \
    -Zbuild-std-features=std/optimize_for_size \
    "$@"

