#!/usr/bin/env bash
set -Eeuo pipefail

source ./scripts/ci/prepare_env.sh > prepare_env.log 2>&1 || { cat prepare_env.log; exit 1; }
./scripts/build.sh -d release -t Release -c gcc -x '-DBUILD_BENCHMARKS=ON' -n > build1.log 2>&1 || { cat build1.log; exit 1; }
cmake --build release --parallel "$(nproc)" --target payments_bench bench_server hog-event control_server control_cli get_port > build2.log 2>&1 || { cat build2.log; exit 1; }

if [[ ${HOG_HOST_KEY} == db ]]; then
  # Build the exact parent's four production files in the same checkout,
  # keeping the client, control service, dependencies, and compiler fixed.
  bench=release/hogd/hog_exec/bench/bench_server
  cp "$bench" "$bench.pr"
  base=9656117d1cfeba2ea5ca61c0a5995f0da2126aec
  git fetch --depth 1 origin "$base" --no-progress --quiet
  files=(hogd/hog_db/{block_tx,invoke_tx}.{h,cc})
  git restore --source="$base" --worktree -- "${files[@]}"
  cmake --build release --parallel "$(nproc)" --target bench_server > build-base.log 2>&1 || { cat build-base.log; exit 1; }
  cp "$bench" "$bench.base"
  git restore --source=HEAD --worktree -- "${files[@]}"
  ln -sfn bench_server.pr "$bench"
  sha256sum "$bench.base" "$bench.pr"
fi
