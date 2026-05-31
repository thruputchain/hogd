#!/bin/sh
# Build the sdk_rs counter-rv32 example and lower it to a hogd smoke artifact.

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET_TRIPLE="riscv32im-unknown-none-elf"
PACKAGE="counter-rv32"
SDK_RS_DIR=""
OUT=""
METADATA=""

usage() {
  cat >&2 <<EOF
usage: $0 --sdk-rs-dir <sdk_rs checkout> --out <artifact.rv32a> [--metadata <metadata.json>]

Builds sdk_rs/example-contracts counter-rv32 and lowers it with:
  ${ROOT_DIR}/scripts/rv32_make_smoke_artifact.sh
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
  --sdk-rs-dir)
    if [ "$#" -lt 2 ]; then
      echo "error: --sdk-rs-dir requires a value" >&2
      exit 1
    fi
    SDK_RS_DIR="${2:-}"
    shift 2
    ;;
  --out)
    if [ "$#" -lt 2 ]; then
      echo "error: --out requires a value" >&2
      exit 1
    fi
    OUT="${2:-}"
    shift 2
    ;;
  --metadata)
    if [ "$#" -lt 2 ]; then
      echo "error: --metadata requires a value" >&2
      exit 1
    fi
    METADATA="${2:-}"
    shift 2
    ;;
  --help | -h)
    usage
    exit 0
    ;;
  *)
    echo "error: unknown argument: $1" >&2
    usage
    exit 1
    ;;
  esac
done

if [ -z "${SDK_RS_DIR}" ] || [ -z "${OUT}" ]; then
  echo "error: --sdk-rs-dir and --out are required" >&2
  usage
  exit 1
fi

SDK_RS_DIR="$(CDPATH= cd -- "${SDK_RS_DIR}" && pwd)"
EXAMPLE_DIR="${SDK_RS_DIR}/example-contracts"
if [ ! -f "${EXAMPLE_DIR}/Cargo.toml" ]; then
  echo "error: sdk_rs example workspace not found: ${EXAMPLE_DIR}" >&2
  exit 1
fi

RUSTC_VERSION="$(cd "${EXAMPLE_DIR}" && rustc --version)"

# Single source of truth for the cargo invocation: the recorded --build-flags
# metadata is derived from the exact env + argv used below, so the two can't
# drift. CARGO_ENV holds `name=value` assignments applied via `export`.
CARGO_ENV="CARGO_PROFILE_RELEASE_STRIP=false"
set -- cargo build --release --locked --no-default-features \
  --features entrypoints --target "${TARGET_TRIPLE}" -p "${PACKAGE}"
BUILD_FLAGS="${CARGO_ENV} $*"

(
  cd "${EXAMPLE_DIR}"
  # shellcheck disable=SC2086
  export ${CARGO_ENV}
  "$@"
)

ELF="${SDK_RS_DIR}/target/${TARGET_TRIPLE}/release/counter_rv32"
if [ ! -f "${ELF}" ]; then
  echo "error: expected RV32 ELF not found: ${ELF}" >&2
  exit 1
fi

set -- "${ROOT_DIR}/scripts/rv32_make_smoke_artifact.sh" \
  --elf "${ELF}" \
  --out "${OUT}" \
  --rustc-version "${RUSTC_VERSION}" \
  --target-triple "${TARGET_TRIPLE}" \
  --build-flags "${BUILD_FLAGS}"

if [ -n "${METADATA}" ]; then
  set -- "$@" --metadata "${METADATA}"
fi

exec "$@"
