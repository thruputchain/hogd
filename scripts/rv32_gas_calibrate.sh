#!/bin/bash
# Build and run the RV32 gas calibration harness.
#
# The harness links only the RV32 ISA core (machine.cc, program.cc) plus the
# header-only threaded interpreter, which by design has no hogd dependencies --
# no OpenSSL, no protocol, no XDR. So it builds standalone in seconds without
# configuring the node build, which is what makes "run the calibration" a thing
# you can actually do on a candidate machine rather than a build session.
#
# The measured engine is ThreadedMachine with tracing off: the same lowering,
# the same handlers, and the same LTO/-O2 codegen production runs.
#
# Usage:
#   scripts/rv32_gas_calibrate.sh [--out PATH] [-- <harness args>]
#
# Examples:
#   scripts/rv32_gas_calibrate.sh --out /tmp/gas.json
#   scripts/rv32_gas_calibrate.sh -- --quick
#   scripts/rv32_gas_calibrate.sh -- --op=DIV --repeats=25

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${REPO_ROOT}/hogd/hog_exec/runtime/rv32"
BUILD_DIR="${RV32_GAS_BUILD_DIR:-${REPO_ROOT}/build-rv32-gas-calibration}"
OUT_PATH=""
HARNESS_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      OUT_PATH="$2"
      shift 2
      ;;
    --out=*)
      OUT_PATH="${1#--out=}"
      shift
      ;;
    --)
      shift
      HARNESS_ARGS=("$@")
      break
      ;;
    *)
      echo "usage: $0 [--out PATH] [-- <harness args>]" >&2
      exit 2
      ;;
  esac
done

CXX="${CXX:-clang++}"
if ! command -v "${CXX}" >/dev/null 2>&1; then
  echo "error: ${CXX} not found; set CXX to a compiler with musttail support" >&2
  echo "       (clang, or GCC >= 15 -- see runtime/rv32/musttail.h)" >&2
  exit 1
fi

# -O2 plus LTO matches how CMake builds the production rv32 objects
# (INTERPROCEDURAL_OPTIMIZATION on rv32_isa_core in the rv32 CMakeLists), so the
# interpreter measured here is the interpreter the node runs. Measuring an
# un-optimized or non-LTO build would price a different engine.
CXXFLAGS=(-std=c++23 -O2 -flto -DNDEBUG -Wall -I "${REPO_ROOT}")

mkdir -p "${BUILD_DIR}"
SOURCES=(
  "${SRC_DIR}/machine.cc"
  "${SRC_DIR}/program.cc"
  "${SRC_DIR}/gas_calibration/kernel.cc"
  "${SRC_DIR}/gas_calibration/lowering_audit.cc"
  "${SRC_DIR}/gas_calibration/measure.cc"
  "${SRC_DIR}/gas_calibration/provenance.cc"
  "${SRC_DIR}/gas_calibration/workloads.cc"
  "${SRC_DIR}/gas_calibration/rv32_gas_calibrate.cc"
)

BINARY="${BUILD_DIR}/rv32_gas_calibrate"
echo "building ${BINARY}..." >&2
"${CXX}" "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "${BINARY}"

if [[ -n "${OUT_PATH}" ]]; then
  HARNESS_ARGS+=("--out=${OUT_PATH}")
fi

# Stamp the source revision into the output. A calibration JSON that cannot say
# which tree it came from cannot be compared with a later one, and comparing
# runs across hardware and across interpreter changes is the point.
if git -C "${REPO_ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
  REV="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
  if ! git -C "${REPO_ROOT}" diff --quiet HEAD 2>/dev/null; then
    REV="${REV}-dirty"
  fi
  export RV32_GAS_SOURCE_REV="${REV}"
fi

# Pin to one core when possible. The estimator is already robust to slow drift
# (points are measured round-robin and reduced by median), but migrating
# between cores mid-sweep adds cache-cold samples that no amount of repeats
# removes.
RUNNER=()
if command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c 0)
fi

echo "running ${BINARY} ${HARNESS_ARGS[*]:-}" >&2
exec "${RUNNER[@]}" "${BINARY}" "${HARNESS_ARGS[@]:-}"
