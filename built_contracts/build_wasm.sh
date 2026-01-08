#!/bin/sh
# Build a WASM contract with controlled environment
# Usage: build_wasm.sh <target_name> <wasm_flags> <target_dir>

set -e

TARGET_NAME="$1"
WASM_FLAGS="$2"
TARGET_DIR="$3"

if [ -z "$TARGET_NAME" ] || [ -z "$TARGET_DIR" ]; then
    echo "Usage: build_wasm.sh <target_name> <wasm_flags> <target_dir>" >&2
    exit 1
fi

# Clean environment
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS

# Set environment variables
export CARGO_NET_GIT_FETCH_WITH_CLI=true
export RUSTFLAGS='-C link-args=-zstack-size=30000 -Zunstable-options -Cpanic=immediate-abort'

# Build the command
CARGO_CMD="cargo build -p ${TARGET_NAME} --target wasm32-unknown-unknown --release --locked --frozen -F guest --offline --target-dir ${TARGET_DIR} -Zbuild-std=core,alloc -Zbuild-std-features=std/optimize_for_size"

# Add wasm_flags if provided
if [ -n "$WASM_FLAGS" ]; then
    CARGO_CMD="${CARGO_CMD} ${WASM_FLAGS}"
fi

# Execute
exec $CARGO_CMD

