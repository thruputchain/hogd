#!/bin/bash
# Build and run the RV32 gas calibration harness against the production engine.
#
# The measured engine is the *compiled* interpreter, so the build configuration
# is part of the measurement, not a detail of it. This drives the project's
# ordinary Release build (scripts/build.sh, which is Release by default) rather
# than compiling by hand: Release is -O3, -march, libc++ under clang, and LTO
# across the interpreter's translation units, and an ad-hoc -O2 build produces
# numbers that look entirely reasonable and describe a different interpreter.
# rv32_gas_calibrate refuses to run unless it was built by that configuration.
#
# Only the rv32_gas_calibrate target is built, and it links just the RV32 ISA
# core, so after the first configure the build is a handful of files.
#
# The build directory is a required argument. It is not searched for and there
# is no default: which tree the binary comes from decides what is measured, and
# a benchmark that picks that for you can measure something other than what you
# asked for without ever saying so.
#
# Usage:
#   scripts/rv32_gas_calibrate.sh BUILD_DIR [--out PATH] [-- <harness args>]
#
# Examples:
#   scripts/rv32_gas_calibrate.sh build-release-ccache-tmp --out /tmp/gas.json
#   scripts/rv32_gas_calibrate.sh build-release-ccache-tmp -- --quick
#   scripts/rv32_gas_calibrate.sh build-release-ccache-tmp -- --op=DIV

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_PATH=""
HARNESS_ARGS=()

usage() {
  echo "usage: $0 BUILD_DIR [--out PATH] [-- <harness args>]" >&2
  echo "       BUILD_DIR is required: an existing Release build tree, or a" >&2
  echo "       path to configure one at." >&2
}

if [[ $# -eq 0 || "$1" == -* ]]; then
  usage
  exit 2
fi
BUILD_DIR="$1"
shift

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
    usage
    exit 2
    ;;
  esac
done

# Resolve the build directory before anything uses it. scripts/build.sh
# interprets its -d argument relative to its own working directory, so a
# relative BUILD_DIR would otherwise mean different trees depending on where
# this script was invoked from -- and the whole point of naming it explicitly is
# that it is unambiguous.
case "${BUILD_DIR}" in
/*) ;;
*) BUILD_DIR="${REPO_ROOT}/${BUILD_DIR}" ;;
esac

CACHE="${BUILD_DIR}/CMakeCache.txt"
# Refuse an existing non-Release tree rather than reconfiguring it: the caller
# named this directory, and silently flipping its build type would break
# whatever else they use it for.
if [[ -f "${CACHE}" ]] &&
  ! grep -q "^CMAKE_BUILD_TYPE:STRING=Release$" "${CACHE}" 2>/dev/null; then
  echo "error: ${BUILD_DIR} is not a Release build directory." >&2
  echo "       Gas costs describe the interpreter the node runs, which is the" >&2
  echo "       Release one. Name a Release tree, or a path to configure a new" >&2
  echo "       one at." >&2
  exit 2
fi

# Build chatter goes to stderr: with no --out the harness writes its JSON to
# stdout, and configure/build output mixed into it would produce a file that is
# not JSON.
echo "building rv32_gas_calibrate (Release) in ${BUILD_DIR}..." >&2
"${REPO_ROOT}/scripts/build.sh" -d "${BUILD_DIR}" -t Release rv32_gas_calibrate >&2

BINARY="${BUILD_DIR}/hogd/hog_exec/runtime/rv32/gas_calibration/rv32_gas_calibrate"
if [[ ! -x "${BINARY}" ]]; then
  echo "error: ${BINARY} was not produced by the build" >&2
  exit 1
fi

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

# Pin the code layout. ASLR redraws handler addresses per process, which moves
# indirect-branch aliasing in a direct-threaded dispatch.
#
# Demonstrated by removing it: unpinned, 25 of 100 cells move over 10% against
# the pinned reference and ADD doubles, and two unpinned runs disagree with
# each other (LH chase 1.70 vs 2.65). Pinned, three runs hold to 0.20% median.
if command -v setarch >/dev/null 2>&1 &&
  setarch "$(uname -m)" -R true >/dev/null 2>&1; then
  RUNNER+=(setarch "$(uname -m)" -R)
fi

exec "${RUNNER[@]}" "${BINARY}" "${HARNESS_ARGS[@]:-}"
