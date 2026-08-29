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
# Usage:
#   scripts/rv32_gas_calibrate.sh [--build-dir DIR] [--out PATH] [-- <harness args>]
#
# Examples:
#   scripts/rv32_gas_calibrate.sh --out /tmp/gas.json
#   scripts/rv32_gas_calibrate.sh -- --quick
#   scripts/rv32_gas_calibrate.sh --build-dir build-release-ccache-tmp -- --op=DIV
#
# Reuses an existing build directory when one is given or found, so a
# calibration on a machine that already builds hogd costs a few files, not a
# configure.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${RV32_GAS_BUILD_DIR:-}"
OUT_PATH=""
HARNESS_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#--build-dir=}"
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
    --)
      shift
      HARNESS_ARGS=("$@")
      break
      ;;
    *)
      echo "usage: $0 [--build-dir DIR] [--out PATH] [-- <harness args>]" >&2
      exit 2
      ;;
  esac
done

# Prefer a Release build directory that already exists: configuring a fresh one
# pulls in the whole project, while reusing one costs only this target's files.
if [[ -z "${BUILD_DIR}" ]]; then
  for candidate in build-release-ccache-tmp build-release build; do
    if [[ -f "${REPO_ROOT}/${candidate}/CMakeCache.txt" ]] &&
      grep -q "^CMAKE_BUILD_TYPE:STRING=Release$" \
        "${REPO_ROOT}/${candidate}/CMakeCache.txt" 2>/dev/null; then
      BUILD_DIR="${candidate}"
      break
    fi
  done
fi
BUILD_DIR="${BUILD_DIR:-build-release-gas}"

CACHE="${REPO_ROOT}/${BUILD_DIR}/CMakeCache.txt"
if [[ -f "${CACHE}" ]] &&
  ! grep -q "^CMAKE_BUILD_TYPE:STRING=Release$" "${CACHE}" 2>/dev/null; then
  echo "error: ${BUILD_DIR} is not a Release build directory." >&2
  echo "       Gas costs describe the interpreter the node runs, which is the" >&2
  echo "       Release one. Point --build-dir at a Release tree, or drop this" >&2
  echo "       one and let this script configure a fresh Release build." >&2
  exit 2
fi

# Build chatter goes to stderr: with no --out the harness writes its JSON to
# stdout, and configure/build output mixed into it would produce a file that is
# not JSON.
echo "building rv32_gas_calibrate (Release) in ${BUILD_DIR}..." >&2
"${REPO_ROOT}/scripts/build.sh" -d "${BUILD_DIR}" -t Release rv32_gas_calibrate >&2

BINARY="${REPO_ROOT}/${BUILD_DIR}/hogd/hog_exec/runtime/rv32/gas_calibration/rv32_gas_calibrate"
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

exec "${RUNNER[@]}" "${BINARY}" "${HARNESS_ARGS[@]:-}"
