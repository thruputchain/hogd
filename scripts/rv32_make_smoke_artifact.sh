#!/bin/sh
# Lower an already-built RV32 SDK contract ELF into a hogd smoke-test artifact.
#
# This intentionally does not build the SDK contract. The caller owns producing
# a riscv32im-unknown-none-elf binary; this wrapper only standardizes the
# published lowerer version used to produce the .rv32a file consumed by
# RV32_SMOKE_ARTIFACT.

set -eu

# Bump this together with docs/rv32/rv32-author-toolchain.md.
LOWERER_VERSION="0.5.5"
EXPECTED_VERSION="rv32-lower ${LOWERER_VERSION}"
INSTALL_HINT="CARGO_REGISTRIES_GROUNDHOG_INDEX=sparse+https://groundhog.dev/crates/api/v1/crates/ cargo install --registry groundhog groundhog-rv32-lower@${LOWERER_VERSION}"

usage() {
  cat >&2 <<EOF
usage: $0 --elf <contract-elf> --out <artifact.rv32a> [--metadata <metadata.json>] [--json]
          [--rustc-version <version>] [--target-triple <triple>] [--build-flags <flags>]

Requires ${EXPECTED_VERSION} on PATH.
Install with:
  ${INSTALL_HINT}
EOF
}

ELF=""
OUT=""
METADATA=""
RUSTC_VERSION=""
TARGET_TRIPLE=""
BUILD_FLAGS=""
JSON=0

while [ "$#" -gt 0 ]; do
  case "$1" in
  --elf)
    if [ "$#" -lt 2 ]; then
      echo "error: --elf requires a value" >&2
      exit 1
    fi
    ELF="${2:-}"
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
  --json)
    JSON=1
    shift
    ;;
  --rustc-version)
    if [ "$#" -lt 2 ]; then
      echo "error: --rustc-version requires a value" >&2
      exit 1
    fi
    RUSTC_VERSION="${2:-}"
    shift 2
    ;;
  --target-triple)
    if [ "$#" -lt 2 ]; then
      echo "error: --target-triple requires a value" >&2
      exit 1
    fi
    TARGET_TRIPLE="${2:-}"
    shift 2
    ;;
  --build-flags)
    if [ "$#" -lt 2 ]; then
      echo "error: --build-flags requires a value" >&2
      exit 1
    fi
    BUILD_FLAGS="${2:-}"
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

if [ -z "${ELF}" ] || [ -z "${OUT}" ]; then
  echo "error: --elf and --out are required" >&2
  usage
  exit 1
fi

if [ ! -f "${ELF}" ]; then
  echo "error: ELF not found: ${ELF}" >&2
  exit 1
fi

RV32_LOWER_BIN="${RV32_LOWER_BIN:-$(command -v rv32-lower || true)}"
if [ -z "${RV32_LOWER_BIN}" ]; then
  echo "error: rv32-lower not found on PATH" >&2
  echo "install with: ${INSTALL_HINT}" >&2
  exit 1
fi
if [ ! -x "${RV32_LOWER_BIN}" ]; then
  echo "error: rv32-lower is not executable: ${RV32_LOWER_BIN}" >&2
  echo "install with: ${INSTALL_HINT}" >&2
  exit 1
fi

ACTUAL_VERSION="$("${RV32_LOWER_BIN}" --version 2>/dev/null || true)"
if [ "${ACTUAL_VERSION}" != "${EXPECTED_VERSION}" ]; then
  echo "error: expected '${EXPECTED_VERSION}', got '${ACTUAL_VERSION:-unknown}'" >&2
  echo "install with: ${INSTALL_HINT}" >&2
  exit 1
fi

mkdir -p "$(dirname "${OUT}")"
if [ -n "${METADATA}" ]; then
  mkdir -p "$(dirname "${METADATA}")"
fi

set -- "${RV32_LOWER_BIN}" --elf "${ELF}" --out "${OUT}"
if [ -n "${METADATA}" ]; then
  set -- "$@" --metadata "${METADATA}"
fi
if [ -n "${RUSTC_VERSION}" ]; then
  set -- "$@" --rustc-version "${RUSTC_VERSION}"
fi
if [ -n "${TARGET_TRIPLE}" ]; then
  set -- "$@" --target-triple "${TARGET_TRIPLE}"
fi
if [ -n "${BUILD_FLAGS}" ]; then
  set -- "$@" --build-flags "${BUILD_FLAGS}"
fi
if [ "${JSON}" = "1" ]; then
  set -- "$@" --json
fi

exec "$@"
