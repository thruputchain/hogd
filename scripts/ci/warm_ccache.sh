#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(realpath "${SCRIPT_DIR}/../..")"

TARGET_SHA="${1:-}"
WARMUP_JOBS="${HOGD_CCACHE_WARMUP_JOBS:-auto}"
BUILD_ROOT="${HOGD_CCACHE_WARMUP_BUILD_ROOT:-build/ccache-warm}"
LOCK_FILE="${HOGD_CCACHE_WARMUP_LOCK:-/tmp/hogd-ccache-warmup.lock}"

detect_warmup_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
    return
  fi

  getconf _NPROCESSORS_ONLN
}

if [[ -z "$TARGET_SHA" ]]; then
  echo "usage: $0 <git-sha>" >&2
  exit 2
fi

if [[ "$WARMUP_JOBS" == "auto" ]]; then
  WARMUP_JOBS="$(detect_warmup_jobs)"
fi

if ! [[ "$WARMUP_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "HOGD_CCACHE_WARMUP_JOBS must be 'auto' or a positive integer, got: $WARMUP_JOBS" >&2
  exit 2
fi

if ! command -v ccache >/dev/null 2>&1; then
  echo "ccache is required for cache warmup" >&2
  exit 1
fi

exec 9>"$LOCK_FILE"
echo "waiting for warmup lock: $LOCK_FILE"
flock 9

cd "$ROOT_DIR"

echo "warming ccache for commit: $TARGET_SHA"
echo "build parallelism: $WARMUP_JOBS"
echo "build root: $BUILD_ROOT"
echo "ccache dir: ${CCACHE_DIR:-<host default>}"
echo "ccache namespace: ${CCACHE_NAMESPACE:-<unset>}"

CURRENT_SHA="$(git rev-parse HEAD)"
if [[ "$CURRENT_SHA" != "$TARGET_SHA" ]]; then
  echo "warmup checkout mismatch: expected $TARGET_SHA, got $CURRENT_SHA" >&2
  exit 1
fi

echo "ccache stats before warmup"
ccache -s || true

FAILURES=0

run_build() {
  local name="$1"
  local compiler="$2"
  local build_type="$3"
  shift 3

  local build_dir="$BUILD_ROOT/$name"
  if [[ -z "$build_dir" || "$build_dir" == "/" || "$build_dir" == "." ]]; then
    echo "refusing to clear unsafe build directory: $build_dir" >&2
    return 1
  fi

  echo
  echo "===== warmup: $name ====="
  echo "compiler: $compiler"
  echo "build type: $build_type"
  echo "cmake extras: ${*:-<none>}"
  echo "clearing build dir: $build_dir"

  rm -rf -- "$build_dir"

  ./scripts/build.sh \
    -d "$build_dir" \
    -t "$build_type" \
    -c "$compiler" \
    -j "$WARMUP_JOBS" \
    "$@"
}

for compiler in gcc clang; do
  for build_type in Release Debug; do
    prefix="${compiler}-${build_type,,}"

    if ! run_build "$prefix-default" "$compiler" "$build_type"; then
      FAILURES=$((FAILURES + 1))
    fi
    if ! run_build "$prefix-x23-coro-trace-on" "$compiler" "$build_type" \
      -x "-DX23_CORO_TRACE=1"; then
      FAILURES=$((FAILURES + 1))
    fi
    if ! run_build "$prefix-x23-coro-trace-off" "$compiler" "$build_type" \
      -x "-DX23_CORO_TRACE=0"; then
      FAILURES=$((FAILURES + 1))
    fi
    if ! run_build "$prefix-test-ffi-no-bench" "$compiler" "$build_type" \
      -x "-DENABLE_TEST_FFI=ON" \
      -x "-DBUILD_BENCHMARKS=OFF"; then
      FAILURES=$((FAILURES + 1))
    fi
  done
done

echo
echo "ccache stats after warmup"
ccache -s || true

if ((FAILURES > 0)); then
  echo "$FAILURES ccache warmup build(s) failed" >&2
  exit 1
fi
