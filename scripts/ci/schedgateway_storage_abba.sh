#!/usr/bin/env bash
# Private diagnostic: production stress-batches geometry, balanced disk/RAM.
set -Eeuo pipefail
[[ $# == 1 && ${GIT_BRANCH:-} == codex/gateway-hog7-storage-abba ]]
[[ ${HOG_HOST:-} == hog7 && ${HOGD_COMBINED_WORK_DIR:-} == /bench-combined/ci_work_dir/ ]]
[[ $(findmnt -rn -o TARGET,FSTYPE -T /bench-combined) == "/bench-combined tmpfs" ]]
[[ $(findmnt -rn -o FSTYPE -T /tmp) == ext4 ]]
evidence="$PWD/.ci/dumps/gateway-storage-abba"
mkdir -m 700 -- "$evidence"
cp /proc/self/status "$evidence/launcher-status.txt"
findmnt -T /tmp >"$evidence/disk-mount.txt"
findmnt -T /bench-combined >"$evidence/ram-mount.txt"
export SCHEDGATEWAY_BENCH_OUTSTANDING=8 SCHEDGATEWAY_BENCH_BATCH=100 SCHEDGATEWAY_BENCH_TRANSACTIONS=8000000
sampler=
data=
cleanup() {
  if [[ -n $sampler ]]; then
    kill "$sampler" 2>/dev/null || true
    wait "$sampler" 2>/dev/null || true
  fi
  if [[ -n $data ]]; then rmdir -- "$data" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
i=0
for storage in disk ram ram disk; do
  i=$((i+1))
  arm="$evidence/$i-$storage"
  mkdir -m 700 -- "$arm"
  root=/tmp
  if [[ $storage == ram ]]; then root=/bench-combined; fi
  data=$(mktemp -d -- "$root/gateway-abba.XXXXXXXX")
  printf '%s\n' "$data" >"$arm/data-root.txt"
  findmnt -T "$data" >"$arm/data-mount.txt"
  df -B1 -- "$data" >"$arm/before.txt"
  output="$arm/output"
  if [[ $i == 1 ]]; then output=$1; fi
  # Wait for the sample subprocess to stop before taking the post-run receipt.
  (while :; do
      date +%s.%N
      df -B1 -- "$data"
      sleep 1
    done) >"$arm/usage.txt" &
  sampler=$!
  status=0
  HOGD_CI_SCHEDGATEWAY_DATA_ROOT="$data" "$(dirname "$0")/schedgateway_bench.sh" "$output" || status=$?
  kill "$sampler" 2>/dev/null || true
  wait "$sampler" 2>/dev/null || true
  sampler=
  df -B1 -- "$data" >"$arm/after.txt"
  printf '%s\n' "$status" >"$arm/exit-status.txt"
  if [[ $i == 1 ]]; then cp -a -- "$output" "$arm/output"; fi
  [[ -O $data && -z $(find "$data" -mindepth 1 -maxdepth 1 -print -quit) ]]
  rmdir -- "$data"
  echo "removed" >"$arm/cleanup.txt"
  [[ $status == 0 ]]
  python3 - "$data" "$arm/output/c8/readiness.json" <<'PY'
import json, sys
from pathlib import Path
assert Path(json.loads(Path(sys.argv[2]).read_text())["manifestPath"]).is_relative_to(sys.argv[1])
PY
  data=
done
