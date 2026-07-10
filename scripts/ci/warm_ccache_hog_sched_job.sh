#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <github-repo> <git-sha> <warmup-job>" >&2
  exit 2
fi

GITHUB_REPO="$1"
TARGET_SHA="$2"
WARMUP_JOB="$3"

case "$WARMUP_JOB" in
ccache_gcc_release_default) WARMUP_ARGS=(gcc Release default) ;;
ccache_gcc_release_x23_coro_trace_on) WARMUP_ARGS=(gcc Release x23-coro-trace-on) ;;
ccache_gcc_release_x23_coro_trace_off) WARMUP_ARGS=(gcc Release x23-coro-trace-off) ;;
ccache_gcc_release_test_ffi_no_bench) WARMUP_ARGS=(gcc Release test-ffi-no-bench) ;;
ccache_gcc_debug_default) WARMUP_ARGS=(gcc Debug default) ;;
ccache_gcc_debug_x23_coro_trace_on) WARMUP_ARGS=(gcc Debug x23-coro-trace-on) ;;
ccache_gcc_debug_x23_coro_trace_off) WARMUP_ARGS=(gcc Debug x23-coro-trace-off) ;;
ccache_gcc_debug_test_ffi_no_bench) WARMUP_ARGS=(gcc Debug test-ffi-no-bench) ;;
ccache_clang_release_default) WARMUP_ARGS=(clang Release default) ;;
ccache_clang_release_x23_coro_trace_on) WARMUP_ARGS=(clang Release x23-coro-trace-on) ;;
ccache_clang_release_x23_coro_trace_off) WARMUP_ARGS=(clang Release x23-coro-trace-off) ;;
ccache_clang_release_test_ffi_no_bench) WARMUP_ARGS=(clang Release test-ffi-no-bench) ;;
ccache_clang_debug_default) WARMUP_ARGS=(clang Debug default) ;;
ccache_clang_debug_x23_coro_trace_on) WARMUP_ARGS=(clang Debug x23-coro-trace-on) ;;
ccache_clang_debug_x23_coro_trace_off) WARMUP_ARGS=(clang Debug x23-coro-trace-off) ;;
ccache_clang_debug_test_ffi_no_bench) WARMUP_ARGS=(clang Debug test-ffi-no-bench) ;;
*)
  echo "unknown ccache warmup job: $WARMUP_JOB" >&2
  exit 2
  ;;
esac

export CARGO_REGISTRIES_GROUNDHOG_INDEX="sparse+https://groundhog.dev/crates/api/v1/crates/"
export CARGO_REGISTRIES_GROUNDHOG_SOURCE="sparse+https://groundhog.dev/crates/api/v1/crates/"

echo 'cloning hogd'
git init -q .
git remote remove origin 2>/dev/null || true
git remote add origin "git@github.com:${GITHUB_REPO}.git"
git fetch --depth 1 origin "$TARGET_SHA" --no-progress --quiet
git checkout -f -q "$TARGET_SHA"
git clean -ffd -e .ssh/ -e .gitconfig

git submodule update --init --recursive --depth 1 --jobs 8 --quiet

source ./scripts/ci/prepare_env.sh >prepare_env.log 2>&1 || (cat prepare_env.log && exit 1)

HOGD_CCACHE_WARMUP_COMPILER="${WARMUP_ARGS[0]}" \
  HOGD_CCACHE_WARMUP_BUILD_TYPE="${WARMUP_ARGS[1]}" \
  HOGD_CCACHE_WARMUP_VARIANT="${WARMUP_ARGS[2]}" \
  ./scripts/ci/warm_ccache.sh "$TARGET_SHA"
