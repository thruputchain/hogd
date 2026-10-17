#!/bin/sh
# Build an RV32 contract via thruput-cli
# Usage: build_risc.sh [--no-spec] <target_name> <target_dir> [additional cargo flags...]
#
# Produces:
#   <target_dir>/<target_name>.rv32a
#   <target_dir>/<target_name>.rv32.json
#   <target_dir>/<target_name>_spec.json  (unless --no-spec)

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# Version gate
EXPECTED_VERSION="thruput 0.30.0"
ACTUAL_VERSION="$(thruput --version 2>/dev/null || true)"
ACTUAL_RELEASE_VERSION="${ACTUAL_VERSION%% (*}"
if [ "${ACTUAL_RELEASE_VERSION}" != "${EXPECTED_VERSION}" ]; then
    echo "error: expected '${EXPECTED_VERSION}', got '${ACTUAL_VERSION:-not installed}'. Run scripts/fetch.sh" >&2
    exit 1
fi

EXPECT_SPEC=1
if [ "${1:-}" = "--no-spec" ]; then
    EXPECT_SPEC=0
    shift
fi

TARGET_NAME="${1:-}"
TARGET_DIR="${2:-}"

if [ -z "${TARGET_NAME}" ] || [ -z "${TARGET_DIR}" ]; then
    echo "Usage: build_risc.sh [--no-spec] <target_name> <target_dir> [additional cargo flags...]" >&2
    exit 1
fi

shift 2

# Clean environment
unset RUSTC_WRAPPER
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS
export CARGO_NET_GIT_FETCH_WITH_CLI=true

# Resolve the output directory to an absolute path before cd'ing into the
# thruput project.
mkdir -p "${TARGET_DIR}"
TARGET_DIR="$(CDPATH= cd -- "${TARGET_DIR}" && pwd)"

# Keep cargo/thruput intermediates in the build tree instead of the source tree.
INTERNAL_TARGET_DIR="${TARGET_DIR}/cargo-target"
# CI sets HOGD_RV32_TARGET_DIR to a directory on its cache volume, so that a
# fresh build tree doesn't recompile the contracts' dependencies and
# build-std's core/alloc from scratch on every run. Each output directory gets
# its own subdirectory: ninja builds the contracts and the RV32 smoke artifact
# in parallel, and they would otherwise wait on each other's cargo lock.
if [ -n "${HOGD_RV32_TARGET_DIR:-}" ]; then
    INTERNAL_TARGET_DIR="${HOGD_RV32_TARGET_DIR}/$(basename "${TARGET_DIR}")"
    # A fresh checkout gives every source file a new mtime, so cargo's default
    # mtime-based freshness would still rebuild every contract. Compare file
    # contents instead; this is nightly-only, as is the contract toolchain.
    export CARGO_UNSTABLE_CHECKSUM_FRESHNESS=true
fi

# Build with thruput-cli (built-in RV32 lowerer). thruput must run inside
# the thruput project, so run from this script's directory regardless of the
# caller's working directory. Pass remaining args through to cargo.
cd "${SCRIPT_DIR}"

# Disable errexit so a build failure reports the contract name below.
# Redirect stderr so cargo progress output doesn't break the jq parse.
BUILD_ERR="$(mktemp)"
trap 'rm -f "${BUILD_ERR}"' EXIT

set +e
OUTPUT=$(thruput build -p "${TARGET_NAME}" --json --target-dir "${INTERNAL_TARGET_DIR}" -- --frozen --offline "$@" 2>"${BUILD_ERR}")
STATUS=$?
set -e

cat "${BUILD_ERR}" >&2

if [ "${STATUS}" -ne 0 ]; then
    echo "error: thruput build failed for ${TARGET_NAME} (exit ${STATUS})" >&2
    printf '%s' "$OUTPUT" | jq -r '.error.message // empty' >&2
    exit 1
fi

# Parse output paths from JSON
RV32_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.rv32')
METADATA_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.metadata')
SPEC_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.specJson')

if [ -z "${RV32_PATH}" ] || [ "${RV32_PATH}" = "null" ]; then
    echo "error: thruput build failed for ${TARGET_NAME}" >&2
    printf '%s' "$OUTPUT" | jq -r '.error.message // empty' >&2
    exit 1
fi

if [ -z "${METADATA_PATH}" ] || [ "${METADATA_PATH}" = "null" ]; then
    echo "error: thruput build did not produce metadata for ${TARGET_NAME}" >&2
    exit 1
fi

# Copy outputs to build directory
cp "${RV32_PATH}" "${TARGET_DIR}/${TARGET_NAME}.rv32a"
cp "${METADATA_PATH}" "${TARGET_DIR}/${TARGET_NAME}.rv32.json"

if [ "${EXPECT_SPEC}" = "1" ]; then
    if [ -z "${SPEC_PATH}" ] || [ "${SPEC_PATH}" = "null" ]; then
        echo "error: thruput build did not produce a spec for ${TARGET_NAME}" >&2
        exit 1
    fi
    cp "${SPEC_PATH}" "${TARGET_DIR}/${TARGET_NAME}_spec.json"
fi
