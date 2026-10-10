#!/usr/bin/env bash
set -Eeuo pipefail

git fetch --depth 1 origin b0d029d74cc837511b2a2c42f94a97a6cd9cc9ad --no-progress --quiet

# Preserve the original benchmark client for the code-generation cross-check.
git restore --source=b0d029d74cc837511b2a2c42f94a97a6cd9cc9ad --worktree -- hogd/hog_exec/bench/payments_bench.cc

source ./scripts/ci/prepare_env.sh >prepare_env.log 2>&1 || {
  cat prepare_env.log
  exit 1
}
./scripts/build.sh -d release -t Release -c gcc -x '-DBUILD_BENCHMARKS=ON' -n >build1.log 2>&1 || {
  cat build1.log
  exit 1
}
cmake --build release --parallel "$(nproc)" --target payments_bench bench_server hog-event control_server control_cli get_port >build2.log 2>&1 || {
  cat build2.log
  exit 1
}

if [[ ${HOG_HOST_KEY} == db || ${HOG_HOST_KEY} == client ]]; then
  # Build the exact parent's four production files in the same checkout,
  # keeping the client, control service, dependencies, and compiler fixed.
  target=bench_server
  if [[ ${HOG_HOST_KEY} == client ]]; then
    target=payments_bench
  fi
  bench=release/hogd/hog_exec/bench/$target
  cp "$bench" "$bench.pr"
  base=9656117d1cfeba2ea5ca61c0a5995f0da2126aec
  git fetch --depth 1 origin "$base" --no-progress --quiet
  files=(hogd/hog_db/{block_tx,invoke_tx}.{h,cc})
  git restore --source="$base" --worktree -- "${files[@]}"
  cmake --build release --parallel "$(nproc)" --target "$target" >build-base.log 2>&1 || {
    cat build-base.log
    exit 1
  }
  cp "$bench" "$bench.base"
  git restore --source=HEAD --worktree -- "${files[@]}"
  ln -sfn "$target.pr" "$bench"
  sha256sum "$bench.base" "$bench.pr"
fi
