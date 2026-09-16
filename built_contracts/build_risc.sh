#!/bin/sh
# Build an RV32 contract via groundhog-cli
# Usage: build_risc.sh [--no-spec] <target_name> <target_dir> [additional cargo flags...]
#
# Produces:
#   <target_dir>/<target_name>.rv32a
#   <target_dir>/<target_name>.rv32.json
#   <target_dir>/<target_name>_spec.json  (unless --no-spec)

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

# Version gate
EXPECTED_VERSION="groundhog 0.17.0"
ACTUAL_VERSION="$(groundhog --version 2>/dev/null || true)"
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
# groundhog project.
mkdir -p "${TARGET_DIR}"
TARGET_DIR="$(CDPATH= cd -- "${TARGET_DIR}" && pwd)"

# Keep cargo/groundhog intermediates in the build tree instead of the source tree.
INTERNAL_TARGET_DIR="${TARGET_DIR}/cargo-target"

# Build with groundhog-cli (built-in RV32 lowerer). groundhog must run inside
# the groundhog project, so run from this script's directory regardless of the
# caller's working directory. Pass remaining args through to cargo.
cd "${SCRIPT_DIR}"

# Run with errexit off so a failure reaches the reporting below: under `set -e`
# the script died on this assignment, so a failed build named neither the
# contract nor groundhog's own error.
# Keep stdout (the --json result) apart from stderr, which carries cargo's
# progress output and would break the jq parse below.
BUILD_ERR="$(mktemp)"
trap 'rm -f "${BUILD_ERR}"' EXIT

set +e
OUTPUT=$(groundhog build -p "${TARGET_NAME}" --json --target-dir "${INTERNAL_TARGET_DIR}" -- --frozen --offline "$@" 2>"${BUILD_ERR}")
STATUS=$?
set -e

cat "${BUILD_ERR}" >&2

if [ "${STATUS}" -ne 0 ]; then
    echo "error: groundhog build failed for ${TARGET_NAME} (exit ${STATUS})" >&2
    printf '%s' "$OUTPUT" | jq -r '.error.message // empty' >&2
    exit 1
fi

# Parse output paths from JSON
RV32_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.rv32')
METADATA_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.metadata')
SPEC_PATH=$(printf '%s' "$OUTPUT" | jq -r '.data.specJson')

if [ -z "${RV32_PATH}" ] || [ "${RV32_PATH}" = "null" ]; then
    echo "error: groundhog build failed for ${TARGET_NAME}" >&2
    printf '%s' "$OUTPUT" | jq -r '.error.message // empty' >&2
    exit 1
fi

if [ -z "${METADATA_PATH}" ] || [ "${METADATA_PATH}" = "null" ]; then
    echo "error: groundhog build did not produce metadata for ${TARGET_NAME}" >&2
    exit 1
fi

# Copy outputs to build directory
cp "${RV32_PATH}" "${TARGET_DIR}/${TARGET_NAME}.rv32a"
cp "${METADATA_PATH}" "${TARGET_DIR}/${TARGET_NAME}.rv32.json"

if [ "${EXPECT_SPEC}" = "1" ]; then
    if [ -z "${SPEC_PATH}" ] || [ "${SPEC_PATH}" = "null" ]; then
        echo "error: groundhog build did not produce a spec for ${TARGET_NAME}" >&2
        exit 1
    fi
    cp "${SPEC_PATH}" "${TARGET_DIR}/${TARGET_NAME}_spec.json"
fi
