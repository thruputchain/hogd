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
hog4) WARMUP_SPECS=("gcc Release default" "gcc Release x23-coro-trace-on") ;;
hog5) WARMUP_SPECS=("gcc Release x23-coro-trace-off" "gcc Release test-ffi-no-bench") ;;
hog6) WARMUP_SPECS=("gcc Debug default" "gcc Debug x23-coro-trace-on") ;;
hog7) WARMUP_SPECS=("gcc Debug x23-coro-trace-off" "gcc Debug test-ffi-no-bench") ;;
hog8) WARMUP_SPECS=("clang Release default" "clang Release x23-coro-trace-on") ;;
hog9) WARMUP_SPECS=("clang Release x23-coro-trace-off" "clang Release test-ffi-no-bench") ;;
hog10) WARMUP_SPECS=("clang Debug default" "clang Debug x23-coro-trace-on") ;;
hog11) WARMUP_SPECS=("clang Debug x23-coro-trace-off" "clang Debug test-ffi-no-bench") ;;
*)
  echo "unknown ccache warmup job: $WARMUP_JOB" >&2
  exit 2
  ;;
esac

export CARGO_REGISTRIES_GROUNDHOG_INDEX="sparse+https://groundhog.dev/crates/api/v1/crates/"
export CARGO_REGISTRIES_GROUNDHOG_SOURCE="sparse+https://groundhog.dev/crates/api/v1/crates/"

echo 'cloning hogd'
# The clone runs on every host of the job at once, and several hog_sched runs
# can be released together the moment the fleet frees up, so a burst of SSH
# connections reaches GitHub in the same second. GitHub answers such a burst by
# closing connections mid-handshake ("Connection closed by 140.82.116.3 port
# 22"), which fails the clone at second one and discards a run that has already
# spent its wait in the queue. Retry the two steps of the clone that talk to
# GitHub; both are idempotent, so a retry resumes rather than restarts. See
# issue #3649.
git_retry() {
  git_retry_attempt=1
  until "$@"; do
    if [ "$git_retry_attempt" -ge 5 ]; then
      echo "clone step failed after $git_retry_attempt attempts: $*" >&2
      return 1
    fi
    # $$ differs per host and per run, so the jitter spreads hosts that
    # collided on one attempt instead of lining them up again on the next.
    sleep $((git_retry_attempt * 15 + $$ % 15))
    git_retry_attempt=$((git_retry_attempt + 1))
  done
}

git init -q .
git remote remove origin 2>/dev/null || true
git remote add origin "git@github.com:${GITHUB_REPO}.git"
git_retry git fetch --depth 1 origin "$TARGET_SHA" --no-progress --quiet
git checkout -f -q "$TARGET_SHA"
git clean -ffd -e .ssh/ -e .gitconfig

git_retry git submodule update --init --recursive --depth 1 --jobs 8 --quiet

source ./scripts/ci/prepare_env.sh >prepare_env.log 2>&1 || (cat prepare_env.log && exit 1)

for spec in "${WARMUP_SPECS[@]}"; do
  read -r warmup_compiler warmup_build_type warmup_variant <<<"$spec"

  HOGD_CCACHE_WARMUP_COMPILER="$warmup_compiler" \
    HOGD_CCACHE_WARMUP_BUILD_TYPE="$warmup_build_type" \
    HOGD_CCACHE_WARMUP_VARIANT="$warmup_variant" \
    ./scripts/ci/warm_ccache.sh "$TARGET_SHA"
done
