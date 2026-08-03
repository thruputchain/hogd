#!/usr/bin/env bash
# wsl_devnode_smoke.sh - assert hog-devnode reaches readiness with a reduced
# ramdb reservation. Run by .github/workflows/wsl-devnode.yml inside a
# memory-constrained WSL2 instance, but works on any Linux host:
#
#   scripts/wsl_devnode_smoke.sh <dir-with-hog-devnode> [ramdb-bytes]
#
# The directory must hold the packaged layout: hog-devnode plus
# contracts/dev_wallet.rv32a (see scripts/package_hogd_validator_release.sh).
set -euo pipefail

dir=${1:?usage: $0 <devnode-dir> [ramdb-bytes]}
ramdb_bytes=${2:-$((4 << 30))}

cd "$dir"
echo "== memory =="
free -h || true
echo "== launching hog-devnode with --ramdb-bytes $ramdb_bytes =="

readiness_file=$(mktemp)
log_file=$(mktemp)
./hog-devnode \
  --listen-host 127.0.0.1 \
  --scheduler-port 0 \
  --readiness stdout \
  --ramdb-bytes "$ramdb_bytes" \
  >"$readiness_file" 2>"$log_file" &
devnode_pid=$!

cleanup() {
  if kill -0 "$devnode_pid" 2>/dev/null; then
    kill "$devnode_pid" 2>/dev/null || true
    for _ in {1..40}; do
      kill -0 "$devnode_pid" 2>/dev/null || break
      sleep 0.25
    done
    kill -9 "$devnode_pid" 2>/dev/null || true
  fi
  wait "$devnode_pid" 2>/dev/null || true
  rm -f "$readiness_file" "$log_file"
}
trap cleanup EXIT

ready=0
for _ in {1..480}; do
  if grep -Eq '"status"[[:space:]]*:[[:space:]]*"ready"' "$readiness_file"; then
    ready=1
    break
  fi
  if ! kill -0 "$devnode_pid" 2>/dev/null; then
    cat "$log_file" >&2
    echo "hog-devnode exited before readiness" >&2
    exit 1
  fi
  sleep 0.25
done

if [ "$ready" -ne 1 ]; then
  cat "$log_file" >&2
  echo "timed out waiting for hog-devnode readiness" >&2
  exit 1
fi

echo "hog-devnode reached readiness with --ramdb-bytes $ramdb_bytes"
