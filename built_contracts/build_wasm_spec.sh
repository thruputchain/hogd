#!/bin/sh
# Build a WASM contract with spec features
# Usage: build_wasm_spec.sh <target_name> <target_dir> [additional cargo flags...]

set -e

TARGET_DIR="$1"
shift
TARGETS="$@"

if [ -z "$TARGET_DIR" ] || [ -z "$TARGETS" ]; then
    echo "Usage: build_wasm_spec.sh <target_dir> <all_targets,...>" >&2
    exit 1
fi

# Clean environment
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS

echo TARGET_DIR: ${TARGET_DIR}
echo TARGETS: ${TARGETS}

exec cargo build \
        --features spec \
        --locked \
        --release \
        --frozen \
        --offline \
        --target-dir ${TARGET_DIR} \
        ${TARGETS}
