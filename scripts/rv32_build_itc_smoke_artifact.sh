#!/bin/sh
# Build the ITC RV32 smoke contract and lower it to a hogd smoke artifact.

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET_TRIPLE="riscv32im-unknown-none-elf"
PACKAGE="rv32_smoke_counter"
BIN="rv32_smoke_counter"
OUT=""
METADATA=""

usage() {
  cat >&2 <<EOF
usage: $0 --out <artifact.rv32a> [--metadata <metadata.json>]

Builds built_contracts/integration_test_contracts/rv32_smoke_counter and lowers it with:
  ${ROOT_DIR}/scripts/rv32_make_smoke_artifact.sh
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
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

if [ -z "${OUT}" ]; then
  echo "error: --out is required" >&2
  usage
  exit 1
fi

BUILD_ROOT="${ROOT_DIR}/built_contracts"
if [ ! -f "${BUILD_ROOT}/Cargo.toml" ]; then
  echo "error: built contracts workspace not found: ${BUILD_ROOT}" >&2
  exit 1
fi

RUSTC_VERSION="$(cd "${BUILD_ROOT}" && rustc --version)"

# Single source of truth for the cargo invocation: the recorded --build-flags
# metadata is derived from the exact env + argv used below, so the two can't
# drift. CARGO_ENV holds `name=value` assignments applied via `export`.
CARGO_ENV="CARGO_PROFILE_RELEASE_STRIP=false"
set -- cargo build -Zbuild-std=core,alloc --release --locked --frozen \
  --offline --no-default-features --features entrypoints \
  --target "${TARGET_TRIPLE}" -p "${PACKAGE}" --bin "${BIN}"
BUILD_FLAGS="${CARGO_ENV} $*"

(
  cd "${BUILD_ROOT}"
  # shellcheck disable=SC2086
  export ${CARGO_ENV}
  "$@"
)

ELF="${BUILD_ROOT}/target/${TARGET_TRIPLE}/release/${BIN}"
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
