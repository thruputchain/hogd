#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "usage: $0 <git-sha> <compiler> <build-type> <variant>" >&2
  exit 2
fi

TARGET_SHA="$1"
WARMUP_COMPILER="$2"
WARMUP_BUILD_TYPE="$3"
WARMUP_VARIANT="$4"

echo 'cloning hogd'
git init -q .
git remote remove origin 2>/dev/null || true
git remote add origin "git@github.com:${GIT_REPO}.git"
git fetch --depth 1 origin "$TARGET_SHA" --no-progress --quiet
git checkout -f -q "$TARGET_SHA"
git clean -ffd -e .ssh/ -e .gitconfig

git submodule update --init --recursive --depth 1 --jobs 8 --quiet

source ./scripts/ci/prepare_env.sh >prepare_env.log 2>&1 || (cat prepare_env.log && exit 1)

HOGD_CCACHE_WARMUP_COMPILER="$WARMUP_COMPILER" \
  HOGD_CCACHE_WARMUP_BUILD_TYPE="$WARMUP_BUILD_TYPE" \
  HOGD_CCACHE_WARMUP_VARIANT="$WARMUP_VARIANT" \
  ./scripts/ci/warm_ccache.sh "$TARGET_SHA"
