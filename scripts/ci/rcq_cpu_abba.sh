#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

if [[ ${GIT_BRANCH:-} != codex/rcq-cpu-abba ||
  ${HOGD_CI_CONTAINER:-} != 1 || ${HOGD_CI_OFFLINE:-} != 1 ||
  ${HOG_HOST:-} != hog10 || ${HOGD_EVENT_NETWORK_DIAGNOSTICS:-} != 1 ||
  ! ${HOG_XID:-} =~ ^hogd-event-bench-[1-9][0-9]*-[1-9][0-9]*$ ||
  ! ${GIT_COMMIT:-} =~ ^[0-9a-f]{40}$ ||
  $(git rev-parse HEAD) != "$GIT_COMMIT" ]]; then
  echo 'RCQ CPU ABBA requires the exact pinned offline hog10 Event diagnostic' >&2
  exit 2
fi
if [[ ${1:-} == --publication-gate ]]; then
  exit 0
fi
[[ $# == 0 && $PWD == /workspace && ${HOG_DUMP_DIR:-} == /workspace/.ci/dumps ]]
build=${BUILD_DIR:-release}
binary="$build/hogd/hog_db/hog_db_bench"
# The Event schedule configures Release but only builds Event and control targets.
grep -Fxq 'CMAKE_BUILD_TYPE:STRING=Release' "$build/CMakeCache.txt"
cmake --build "$build" --parallel 8 --target hog_db_bench
strings "$binary" | grep -Fx "$GIT_COMMIT" >/dev/null

. "$(dirname "$0")/bench_harness.sh"
ci_wait_control_server "./$build/hogd/hog_exec/bench/control_cli" hog11 11111

out="$HOG_DUMP_DIR/rcq-cpu-abba"
mkdir -p "$HOG_DUMP_DIR"
mkdir "$out"
filesystem=$(findmnt -n -o FSTYPE -T /workspace)
[[ $filesystem == ext4 || $filesystem == xfs ]]
python3 - "$GIT_COMMIT" "$HOG_XID" "$filesystem" "$binary" <<'PY' >"$out/provenance.json"
import hashlib
import json
import os
from pathlib import Path
import sys

source, xid, fs, binary = sys.argv[1:]
with open(binary, "rb") as handle:
    digest = hashlib.file_digest(handle, "sha256").hexdigest()
assert os.sched_getaffinity(0) == set(range(128)), "expected full inherited mask 0-127"
rows = {}
for cpu in range(128):
    path = Path(f"/sys/devices/system/cpu/cpu{cpu}")
    [node] = path.glob("node[0-9]*")
    [llc] = [entry for entry in (path / "cache").glob("index*")
             if (entry / "level").read_text().strip() == "3"]
    rows[cpu] = {"node": node.name, "llc": (llc / "shared_cpu_list").read_text().strip(),
                 "siblings": (path / "topology/thread_siblings_list").read_text().strip()}
compact = [rows[cpu] for cpu in range(24)]
assert len({row["siblings"] for row in compact}) == 24
assert len({row["llc"] for row in compact}) == 3
assert len({row["node"] for row in compact}) == 1
print(json.dumps(dict(source=source, xid=xid, host="hog10", filesystem=fs,
                      binary_sha256=digest, topology=rows), indent=2))
PY

run_arm() (
  local arm=$1 mask=$2 prefix=$3 data
  data=$(mktemp -d /workspace/.ci/rcq-cpu-data.XXXXXXXX)
  trap 'rm -rf -- "$data"' EXIT
  [[ $(findmnt -n -o FSTYPE -T "$data") == "$filesystem" ]]
  taskset -c "$mask" bash -euc '
    [[ $(sed -n "s/^Cpus_allowed_list:[[:space:]]*//p" /proc/self/status) == "$1" ]]
    printf "%s\t%s\t%s\n" "$1" "$2" "$3" >"$4"
    exec timeout --kill-after=20s 360s "$5" "$2" rcq_concurrent -n 1000000 -w 24
  ' bash "$mask" "$data" "$arm" "$prefix.proof" "$binary" \
    2>"$prefix.stderr" | sed 's/^/Concurrent: /' | tee "$prefix.stdout"
  grep -Fxq 'Concurrent: validation passed: all queues empty' "$prefix.stdout"
  grep -q 'concurrent benchmark: 12p / 12c, 1 queues, 4000 blocks' "$prefix.stderr"
)

run_arm stock 0-127 "$out/1-stock"
run_arm compact 0-23 "$out/2-compact"
run_arm compact 0-23 "$out/3-compact"
run_arm stock 0-127 "$out/4-stock"
sha256sum "$binary" | cut -d ' ' -f 1 >"$out/binary-after.sha256"
