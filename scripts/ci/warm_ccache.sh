#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(realpath "${SCRIPT_DIR}/../..")"

TARGET_SHA="${1:-}"
WARMUP_JOBS="${HOGD_CCACHE_WARMUP_JOBS:-auto}"
MAX_AUTO_JOBS="${HOGD_CCACHE_WARMUP_MAX_AUTO_JOBS:-96}"
BUILD_ROOT="${HOGD_CCACHE_WARMUP_BUILD_ROOT:-build/ccache-warm}"
LOCK_FILE="${HOGD_CCACHE_WARMUP_LOCK:-/tmp/hogd-ccache-warmup.lock}"
SELECTED_COMPILER="${HOGD_CCACHE_WARMUP_COMPILER:-}"
SELECTED_BUILD_TYPE="${HOGD_CCACHE_WARMUP_BUILD_TYPE:-}"
SELECTED_VARIANT="${HOGD_CCACHE_WARMUP_VARIANT:-}"

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
  if ! [[ "$MAX_AUTO_JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "HOGD_CCACHE_WARMUP_MAX_AUTO_JOBS must be a positive integer, got: $MAX_AUTO_JOBS" >&2
    exit 2
  fi
  if ((WARMUP_JOBS > MAX_AUTO_JOBS)); then
    WARMUP_JOBS="$MAX_AUTO_JOBS"
  fi
fi

if ! [[ "$WARMUP_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "HOGD_CCACHE_WARMUP_JOBS must be 'auto' or a positive integer, got: $WARMUP_JOBS" >&2
  exit 2
fi

if [[ -n "$SELECTED_COMPILER$SELECTED_BUILD_TYPE$SELECTED_VARIANT" ]]; then
  if [[ -z "$SELECTED_COMPILER" || -z "$SELECTED_BUILD_TYPE" || -z "$SELECTED_VARIANT" ]]; then
    echo "HOGD_CCACHE_WARMUP_COMPILER, HOGD_CCACHE_WARMUP_BUILD_TYPE, and HOGD_CCACHE_WARMUP_VARIANT must be set together" >&2
    exit 2
  fi
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
echo "max auto parallelism: $MAX_AUTO_JOBS"
echo "build root: $BUILD_ROOT"
echo "selected compiler: ${SELECTED_COMPILER:-<all>}"
echo "selected build type: ${SELECTED_BUILD_TYPE:-<all>}"
echo "selected variant: ${SELECTED_VARIANT:-<all>}"
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

run_variant() {
  local compiler="$1"
  local build_type="$2"
  local variant="$3"
  local prefix="${compiler}-${build_type,,}"

  case "$compiler" in
  gcc | clang) ;;
  *)
    echo "unsupported HOGD_CCACHE_WARMUP_COMPILER: $compiler" >&2
    return 2
    ;;
  esac

  case "$build_type" in
  Release | Debug) ;;
  *)
    echo "unsupported HOGD_CCACHE_WARMUP_BUILD_TYPE: $build_type" >&2
    return 2
    ;;
  esac

  case "$variant" in
  default)
    run_build "$prefix-default" "$compiler" "$build_type"
    ;;
  x23-coro-trace-on)
    run_build "$prefix-x23-coro-trace-on" "$compiler" "$build_type" \
      -x "-DX23_CORO_TRACE=1"
    ;;
  x23-coro-trace-off)
    run_build "$prefix-x23-coro-trace-off" "$compiler" "$build_type" \
      -x "-DX23_CORO_TRACE=0"
    ;;
  test-ffi-no-bench)
    run_build "$prefix-test-ffi-no-bench" "$compiler" "$build_type" \
      -x "-DENABLE_TEST_FFI=ON" \
      -x "-DBUILD_BENCHMARKS=OFF"
    ;;
  *)
    echo "unsupported HOGD_CCACHE_WARMUP_VARIANT: $variant" >&2
    return 2
    ;;
  esac
}

if [[ -n "$SELECTED_COMPILER" ]]; then
  run_variant "$SELECTED_COMPILER" "$SELECTED_BUILD_TYPE" "$SELECTED_VARIANT"

  echo
  echo "ccache stats after warmup"
  ccache -s || true
  exit 0
fi

for compiler in gcc clang; do
  for build_type in Release Debug; do
    for variant in default x23-coro-trace-on x23-coro-trace-off test-ffi-no-bench; do
      if ! run_variant "$compiler" "$build_type" "$variant"; then
        FAILURES=$((FAILURES + 1))
      fi
    done
  done
done

echo
echo "ccache stats after warmup"
ccache -s || true

if ((FAILURES > 0)); then
  echo "$FAILURES ccache warmup build(s) failed" >&2
  exit 1
fi
