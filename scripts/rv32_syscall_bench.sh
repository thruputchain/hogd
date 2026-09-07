#!/bin/bash
# Build and run the RV32 syscall benchmark against the production syscall path.
#
# The companion to scripts/rv32_gas_calibrate.sh, and split from it for the same
# reason the binaries are: the gas calibration links only the RV32 ISA core so it
# can run on a candidate validator without configuring the node build, and
# syscall_entry needs the protocol XDR, the DB proxy and the snapshot cache. This
# script drives the half that needs the node.
#
# What it measures is the compiled syscall path, so the build configuration is
# part of the measurement rather than a detail of it. This drives the project's
# ordinary Release build; rv32_syscall_entry_bench refuses to run unless it was
# built by that configuration.
#
# The build directory is a required argument. It is not searched for and there
# is no default: which tree the binary comes from decides what is measured, and
# a benchmark that picks that for you can measure something other than what you
# asked for without ever saying so.
#
# Usage:
#   scripts/rv32_syscall_bench.sh BUILD_DIR [--out PATH] [--repetitions N]
#                                 [-- <benchmark args>]
#
# Examples:
#   scripts/rv32_syscall_bench.sh build-release-ccache-tmp --out /tmp/sys.json
#   scripts/rv32_syscall_bench.sh build-release-ccache-tmp -- --benchmark_filter=entry/
#
# Read the output with, and turn it into gas costs with:
#   scripts/rv32_syscall_report.py /tmp/sys.json
#   scripts/rv32_syscall_report.py /tmp/sys.json --schedule --gas /tmp/gas.json
#
# The --gas argument is an opcode calibration JSON from the same host and the
# same engine. It is what converts nanoseconds into gas: a syscall's price has
# to be in the same unit as an instruction's, and that unit is the baseline
# instruction the calibration measured.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_PATH=""
# Odd, so the median the report reads is a sample rather than an interpolation.
# Two ceilings on raising it: the cold row draws kColdIterations single-use keys
# per repetition from a pool of kColdPoolKeys, which runs out past ~13; and the
# calibration job's own hour, most of which is the build.
REPETITIONS=9
BENCH_ARGS=()

usage() {
  echo "usage: $0 BUILD_DIR [--out PATH] [--repetitions N]" \
    "[-- <benchmark args>]" >&2
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
  --repetitions)
    REPETITIONS="$2"
    shift 2
    ;;
  --repetitions=*)
    REPETITIONS="${1#--repetitions=}"
    shift
    ;;
  --)
    shift
    BENCH_ARGS=("$@")
    break
    ;;
  *)
    usage
    exit 2
    ;;
  esac
done

# Resolve before use: scripts/build.sh interprets -d relative to its own working
# directory, so a relative BUILD_DIR would mean different trees depending on
# where this was invoked from.
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
  echo "       Syscall costs describe the path the node runs, which is the" >&2
  echo "       Release one. Name a Release tree, or a path to configure a" >&2
  echo "       new one at." >&2
  exit 2
fi

# Build chatter to stderr: with no --out the benchmark writes to stdout, and
# configure/build output mixed in would produce a file that is not JSON.
echo "building rv32_syscall_entry_bench (Release) in ${BUILD_DIR}..." >&2
"${REPO_ROOT}/scripts/build.sh" -d "${BUILD_DIR}" -t Release \
  -x "-DBUILD_BENCHMARKS=ON" rv32_syscall_entry_bench >&2

BINARY="${BUILD_DIR}/hogd/hog_exec/runtime/rv32_syscall_entry_bench"
if [[ ! -x "${BINARY}" ]]; then
  echo "error: ${BINARY} was not produced by the build" >&2
  exit 1
fi

# Stamp the source revision into the output, the same way the gas calibration
# does. A measurement that cannot say which tree it came from cannot be compared
# with a later one, and comparing across engine changes is the point.
if git -C "${REPO_ROOT}" rev-parse --git-dir >/dev/null 2>&1; then
  REV="$(git -C "${REPO_ROOT}" rev-parse HEAD)"
  if ! git -C "${REPO_ROOT}" diff --quiet HEAD 2>/dev/null; then
    REV="${REV}-dirty"
  fi
  export RV32_GAS_SOURCE_REV="${REV}"
fi

# Pin to one core. The syscall path is a single thread of work and migrating
# between cores mid-run adds cache-cold samples no amount of repetitions
# removes.
RUNNER=()
if command -v taskset >/dev/null 2>&1; then
  RUNNER=(taskset -c 0)
fi

# Pin the code layout. ASLR moves indirect-branch aliasing, which the
# calibration harness measured as worth up to 2x on a direct-threaded dispatch;
# the syscall path is less sensitive but there is no reason to leave it in.
if command -v setarch >/dev/null 2>&1 &&
  setarch "$(uname -m)" -R true >/dev/null 2>&1; then
  RUNNER+=(setarch "$(uname -m)" -R)
fi

# Aggregates only: the report reads medians, and the per-repetition rows are
# noise in a file meant to be committed to a metrics history.
ARGS=(--benchmark_repetitions="${REPETITIONS}"
  --benchmark_report_aggregates_only=true
  --benchmark_min_time=0.5s)
if [[ -n "${OUT_PATH}" ]]; then
  ARGS+=(--benchmark_out="${OUT_PATH}" --benchmark_out_format=json)
else
  ARGS+=(--benchmark_format=json)
fi

exec "${RUNNER[@]}" "${BINARY}" "${ARGS[@]}" ${BENCH_ARGS[0]+"${BENCH_ARGS[@]}"}
