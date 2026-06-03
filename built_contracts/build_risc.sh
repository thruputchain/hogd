#!/bin/sh
# Build an RV32 contract and lower it to a hogd smoke-test artifact.
# Usage: build_risc.sh <target_name> <target_dir> [additional cargo flags...]
#
# Produces:
#   <target_dir>/<target_name>.rv32a
#   <target_dir>/<target_name>.rv32.json

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET_TRIPLE="riscv32im-unknown-none-elf"
TARGET_NAME="${1:-}"
TARGET_DIR="${2:-}"

if [ -z "${TARGET_NAME}" ] || [ -z "${TARGET_DIR}" ]; then
  echo "Usage: build_risc.sh <target_name> <target_dir> [additional cargo flags...]" >&2
  exit 1
fi

shift 2

# Clean environment
unset RUSTC_WRAPPER
unset RUSTFLAGS
unset CARGO_ENCODED_RUSTFLAGS
export CARGO_NET_GIT_FETCH_WITH_CLI=true

mkdir -p "${TARGET_DIR}"

# Keep cargo intermediates in the build tree instead of the source tree.
INTERNAL_TARGET_DIR="${TARGET_DIR}/cargo-target"
ELF="${INTERNAL_TARGET_DIR}/${TARGET_TRIPLE}/release/${TARGET_NAME}"
OUT="${TARGET_DIR}/${TARGET_NAME}.rv32a"
METADATA="${TARGET_DIR}/${TARGET_NAME}.rv32.json"
RUSTC_VERSION="$(cd "${SCRIPT_DIR}" && rustc --version)"

# Single source of truth for the cargo invocation: the recorded --build-flags
# metadata is derived from the exact argv used below. The v4 lowerer no longer
# scans .symtab for entrypoints, so a normal (stripped) release build is a valid
# input -- no CARGO_PROFILE_RELEASE_STRIP override needed.
set -- cargo build -Zbuild-std=core,alloc --release --locked --frozen \
  --offline --no-default-features --features entrypoints \
  --target "${TARGET_TRIPLE}" --target-dir "${INTERNAL_TARGET_DIR}" \
  -p "${TARGET_NAME}" --bin "${TARGET_NAME}" "$@"
BUILD_FLAGS="$*"

(
  cd "${SCRIPT_DIR}"
  "$@"
)

if [ ! -f "${ELF}" ]; then
  echo "error: expected RV32 ELF not found: ${ELF}" >&2
  exit 1
fi

exec "${ROOT_DIR}/scripts/rv32_make_smoke_artifact.sh" \
  --elf "${ELF}" \
  --out "${OUT}" \
  --metadata "${METADATA}" \
  --rustc-version "${RUSTC_VERSION}" \
  --target-triple "${TARGET_TRIPLE}" \
  --build-flags "${BUILD_FLAGS}"
