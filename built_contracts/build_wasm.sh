#!/bin/sh
# Build a WASM contract via groundhog-cli
# Usage: build_wasm.sh [--no-spec] <target_name> <target_dir> [additional cargo flags...]
#
# Produces:
#   <target_dir>/<target_name>.wasm
#   <target_dir>/<target_name>_spec.json  (unless --no-spec)

set -e

# Version gate
EXPECTED_VERSION="groundhog 0.2.2"
ACTUAL_VERSION="$(groundhog --version 2>/dev/null || true)"
if [ "$ACTUAL_VERSION" != "$EXPECTED_VERSION" ]; then
    echo "error: expected '$EXPECTED_VERSION', got '${ACTUAL_VERSION:-not installed}'. Run scripts/fetch.sh" >&2
    exit 1
fi

EXPECT_SPEC=1
if [ "$1" = "--no-spec" ]; then
    EXPECT_SPEC=0
    shift
fi

TARGET_NAME="$1"
TARGET_DIR="$2"

if [ -z "$TARGET_NAME" ] || [ -z "$TARGET_DIR" ]; then
    echo "Usage: build_wasm.sh [--no-spec] <target_name> <target_dir> [additional cargo flags...]" >&2
    exit 1
fi

shift 2

# Clean environment
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS
export CARGO_NET_GIT_FETCH_WITH_CLI=true

# Build with groundhog-cli
# Pass remaining args (e.g. -F features) through to cargo
OUTPUT=$(groundhog build -p "${TARGET_NAME}" --optimize --json -- --frozen --offline "$@")

# Parse output paths from JSON
WASM_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.wasm')
SPEC_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.spec_json')

if [ -z "$WASM_PATH" ] || [ "$WASM_PATH" = "null" ]; then
    echo "error: groundhog build failed for ${TARGET_NAME}" >&2
    printf '%s' "$OUTPUT" | jq -r '.error.message // empty' >&2
    exit 1
fi

# Copy outputs to build directory
mkdir -p "${TARGET_DIR}"
cp "${WASM_PATH}" "${TARGET_DIR}/${TARGET_NAME}.wasm"

if [ "$EXPECT_SPEC" = "1" ]; then
    if [ -z "$SPEC_PATH" ] || [ "$SPEC_PATH" = "null" ]; then
        echo "error: groundhog build did not produce a spec for ${TARGET_NAME}" >&2
        exit 1
    fi
    cp "${SPEC_PATH}" "${TARGET_DIR}/${TARGET_NAME}_spec.json"
fi
