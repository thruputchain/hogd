#!/bin/bash
# Check engine identity, relative costs, workload coverage, and interaction
# bounds against the committed reference. Ratios are specific to a host class;
# a passing check is not an absolute execution-time budget. See
# hogd/hog_exec/runtime/rv32/gas_calibration/README.md.
#
# Usage:
#   scripts/rv32_gas_check.sh BUILD_DIR [--out PATH] [--reference PATH]
#   scripts/rv32_gas_check.sh BUILD_DIR --fingerprint-only
#
# Review changed costs before regenerating a reference:
#   scripts/rv32_gas_calibrate.sh BUILD_DIR --out /tmp/gas.json
#   scripts/rv32_gas_report.py /tmp/gas.json --emit-reference \
#     > hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REFERENCE="${REPO_ROOT}/hogd/hog_exec/runtime/rv32/gas_calibration/reference.txt"
OUT_PATH=""
FINGERPRINT_ONLY="OFF"
CALIBRATE_ARGS=()

usage() {
  echo "usage: $0 BUILD_DIR [--fingerprint-only] [--out PATH]" \
    "[--reference PATH]" >&2
}

if [[ $# -eq 0 || "$1" == -* ]]; then
  usage
  exit 2
fi
BUILD_DIR="$1"
shift

while [[ $# -gt 0 ]]; do
  case "$1" in
  --fingerprint-only)
    FINGERPRINT_ONLY="ON"
    shift
    ;;
  --out)
    OUT_PATH="$2"
    shift 2
    ;;
  --out=*)
    OUT_PATH="${1#--out=}"
    shift
    ;;
  --reference)
    REFERENCE="$2"
    shift 2
    ;;
  --reference=*)
    REFERENCE="${1#--reference=}"
    shift
    ;;
  --)
    shift
    CALIBRATE_ARGS=("$@")
    break
    ;;
  *)
    usage
    exit 2
    ;;
  esac
done

if [[ ! -f "${REFERENCE}" ]]; then
  echo "error: no reference at ${REFERENCE}" >&2
  echo "       generate one with scripts/rv32_gas_report.py --emit-reference" >&2
  exit 2
fi

if [[ "${FINGERPRINT_ONLY}" = "ON" ]]; then
  # Just the deterministic arm: useful by hand after touching the interpreter,
  # when you want the answer now and do not want to wait for a full run.
  ACTUAL="$("${REPO_ROOT}/scripts/rv32_gas_calibrate.sh" "${BUILD_DIR}" \
    -- --fingerprint 2>/dev/null)"
  EXPECTED="$(awk '$1 == "engine_fingerprint" { print $2 }' "${REFERENCE}")"
  if [[ -z "${ACTUAL}" ]]; then
    echo "FAIL  could not compute the engine fingerprint" >&2
    exit 1
  fi
  if [[ "${ACTUAL}" != "${EXPECTED}" ]]; then
    echo "FAIL  engine changed: fingerprint ${ACTUAL} != reference ${EXPECTED}"
    echo "      The lowering's fusion behaviour, gas schedule, or run limits"
    echo "      moved. The gas costs in program.h were calibrated against a"
    echo "      different interpreter. Re-run scripts/rv32_gas_calibrate.sh and"
    echo "      review the costs before regenerating the reference."
    exit 1
  fi
  echo "PASS  engine fingerprint ${ACTUAL} matches the reference."
  exit 0
fi

if [[ -z "${OUT_PATH}" ]]; then
  OUT_PATH="$(mktemp -t rv32-gas-XXXXXX.json)"
  trap 'rm -f "${OUT_PATH}"' EXIT
fi

# ${array[@]:-} on an empty array expands to one empty argument, which the
# harness rejects as an unknown flag; expand to nothing instead.
"${REPO_ROOT}/scripts/rv32_gas_calibrate.sh" "${BUILD_DIR}" --out "${OUT_PATH}" \
  ${CALIBRATE_ARGS[0]+-- "${CALIBRATE_ARGS[@]}"}
exec "${REPO_ROOT}/scripts/rv32_gas_report.py" "${OUT_PATH}" --check "${REFERENCE}"
