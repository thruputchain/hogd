#!/usr/bin/env bash
# Compare storage in a single Combined Metrics bench container.
set -euo pipefail

[[ ${GIT_BRANCH:-} == codex/hog-db-storage &&
   ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
   ${HOG_HOST:-} == hog11 && ${HOG_DUMP_DIR:-} == /workspace/.ci/dumps &&
   $PWD == /workspace && $(git rev-parse HEAD) == "${GIT_COMMIT:?}" ]]

build=release
db_bin=$build/hogd/hog_db/hog_db_bench
node_bin=$build/hogd/devnode/hog-devnode
gateway_bin=$build/hogd/hog_mempool/test/schedgateway_client_bench
[[ -x $db_bin && -x $node_bin && -x $gateway_bin ]]
if [[ $(findmnt -n -o FSTYPE -T /workspace) != ext4 ||
      $(findmnt -n -o FSTYPE -T /bench-event/ci_work_dir) != tmpfs ]]; then
  echo 'Expected ext4 at /workspace and tmpfs at /bench-event' >&2
  exit 1
fi
affinity=$(awk '$1 == "Cpus_allowed_list:" { print $2 }' /proc/self/status)
[[ $affinity =~ ^[0-9,-]+$ ]]

output=$(mktemp -d -- "$HOG_DUMP_DIR/hog-db-storage.XXXXXXXX")
printf 'Hog DB storage trial results: %s\n' "$output"
{
  printf 'host=%s\ncommit=%s\naffinity=%s\nlogical_cpus=%s\n' \
    "$HOG_HOST" "$GIT_COMMIT" "$affinity" "$(nproc)"
  stat -f -c 'path=%n filesystem=%T fsid=%i' /workspace /bench-event/ci_work_dir
  df -Pk /workspace /bench-event
  lscpu -e=CPU,CORE,SOCKET,NODE
} >"$output/host.txt"
sha256sum "$db_bin" "$node_bin" "$gateway_bin" \
  scripts/metrics/hog_db_bench{,_common,_new_storage}.sh \
  scripts/ci/schedgateway_bench.sh hogd/hog_mempool/test/run_schedgateway_bench.py >"$output/sha256sums"
for binary in "$db_bin" "$node_bin" "$gateway_bin"; do
  ldd "$binary" >"$output/${binary##*/}.ldd"
done
awk '{ for (i=1; i<=NF; i++) if (substr($i,1,1)=="/") print $i }' "$output"/*.ldd |
  sort -u | xargs -r sha256sum >"$output/library-sha256sums"

bench_idle() {
  local status=0
  # Linux comm names are at most 15 bytes.
  pgrep -x 'hog-event|hog_db_bench|hog-db|hog-indexer|hog-devnode|schedgateway_cl|hog-mempool|hog-exec|hog-auth' \
    >"$output/unexpected-processes.txt" || status=$?
  if (( status != 1 )); then
    echo 'hog11 is not idle for the storage comparison' >&2
    return 1
  fi
}

run_arm() (
  suite=$1 order=$2 storage=$3
  mount=/workspace
  filesystem=ext4
  if [[ $storage == tmpfs ]]; then
    mount=/bench-event
    filesystem=tmpfs
  fi
  arm="$output/$suite-$order-$storage"
  mkdir -- "$arm"
  bench_idle
  free_kib=$(df -Pk -- "$mount" | awk 'NR==2 {print $4}')
  [[ $free_kib =~ ^[0-9]+$ ]] && (( free_kib >= 4194304 ))
  data=$(mktemp -d -- "$mount/hog-db-storage.XXXXXXXX")
  # Reclaim only this arm's directory, after confirming the writers stopped.
  cleanup_arm() {
    local status=$?
    if bench_idle; then
      rm -rf --one-file-system -- "$data" || status=1
    else
      printf 'Retaining storage trial directory with active writers: %s\n' "$data" >&2
      status=1
    fi
    if (( status == 0 )); then printf 'completed\n' >"$arm/status"; fi
    exit "$status"
  }
  trap cleanup_arm EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  [[ $(findmnt -n -o FSTYPE -T "$data") == "$filesystem" ]]
  [[ $(awk '$1 == "Cpus_allowed_list:" { print $2 }' /proc/self/status) == "$affinity" ]]
  {
    printf 'suite=%s\narm=%s\nstorage=%s\nmount_type=%s\naffinity=%s\n' \
      "$suite" "$order" "$storage" "$filesystem" "$affinity"
    stat -f -c 'path=%n filesystem=%T fsid=%i' "$data"
    df -Pk -- "$data"
  } >"$arm/storage.txt"
  sha256sum --check --status "$output/sha256sums"
  if [[ $suite == hog-db ]]; then
    for variant in hog_db_bench hog_db_bench_new_storage; do
      HOGD_CI_DB_BENCH_ROOT="$data" timeout --kill-after=5s 90s \
        taskset -c "$affinity" bash "scripts/metrics/$variant.sh" "$build" \
        >"$arm/$variant.log" 2>&1
    done
  else
    HOGD_COMBINED_WORK_DIR="$data" BUILD_DIR="$build" \
      SCHEDGATEWAY_BENCH_CONNECTIONS=8 SCHEDGATEWAY_BENCH_OUTSTANDING=8 \
      SCHEDGATEWAY_BENCH_BATCH=100 SCHEDGATEWAY_BENCH_TRANSACTIONS=8000000 \
      SCHEDGATEWAY_BENCH_SECONDS=10 SCHEDGATEWAY_BENCH_WARMUP=2 \
      SCHEDGATEWAY_BENCH_WORKERS=32 SCHEDGATEWAY_BENCH_CLIENT_WORKERS=32 \
      SCHEDGATEWAY_BENCH_ACCOUNTS=1000 timeout --kill-after=20s 720s \
      taskset -c "$affinity" bash scripts/ci/schedgateway_bench.sh "$arm/schedgateway" \
      >"$arm/schedgateway.log" 2>&1
    python3 - "$arm/schedgateway/c8/readiness.json" "$data" <<'PYTHON'
import json
import sys
from pathlib import Path

ready, data = map(Path, sys.argv[1:])
manifest = Path(json.loads(ready.read_text())["manifestPath"])
if data not in manifest.parents:
    raise SystemExit(f"Schedgateway storage is outside the arm's data root: {manifest}")
PYTHON
  fi
  sha256sum --check --status "$output/sha256sums"
)

for suite in hog-db schedgateway; do
  index=0
  for backing in disk tmpfs tmpfs disk; do
    index=$((index+1))
    printf 'Running %s storage arm %s (%s)\n' "$suite" "$index" "$backing"
    run_arm "$suite" "$index" "$backing"
  done
done
