#!/bin/bash
# Periodic check that the RV32 gas calibration still describes the engine.
#
# The way a gas calibration goes wrong is not that it fails -- it is that the
# interpreter moves underneath it and the numbers keep looking reasonable.
# Nothing about a stale calibration looks wrong, so it has to be checked
# mechanically rather than remembered.
#
# Meant for a periodic job -- nightly, weekly, whatever cadence suits -- on a
# reasonably quiet machine. It runs a full calibration through the project's
# Release build (a few minutes; the harness refuses to measure any other build)
# and checks two things against the committed reference:
#
#   1. The engine fingerprint: fusion rules, gas schedule, run limits. Pure
#      equality, no timing, cannot flake. This catches the likeliest failure --
#      someone edits the lowering or a gas cost and the calibration silently
#      keeps describing the old interpreter.
#
#   2. The guarded costs: how expensive the worst cases are *relative to the
#      baseline instruction measured in the same run*. Ratios rather than
#      nanoseconds, so the check survives moving to different hardware, and
#      only cells that reproduce tightly are guarded at all.
#
# The timed tolerance is deliberately loose -- a guarded cost has to double
# before it fires. This is an alarm for things being wildly out of whack, not a
# performance budget; a job that cries wolf gets muted and then catches nothing.
#
# Exit status is 0 when the calibration still holds and 1 when it does not, so
# it drops straight into cron.
#
# The build directory is a required argument, forwarded to
# scripts/rv32_gas_calibrate.sh. It is not searched for and there is no default:
# which tree the binary comes from decides what is measured.
#
# Usage:
#   scripts/rv32_gas_check.sh BUILD_DIR                    # the periodic check
#   scripts/rv32_gas_check.sh BUILD_DIR --out /tmp/gas.json
#   scripts/rv32_gas_check.sh BUILD_DIR --fingerprint-only # "did the engine change?", instant
#
# When it fails, the fix is to understand what moved -- not to regenerate the
# reference. Regenerating is correct only once the new costs have been reviewed
# and, if the schedule needs to follow them, changed:
#
#   scripts/rv32_gas_calibrate.sh --out /tmp/gas.json
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
