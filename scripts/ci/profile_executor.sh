#!/usr/bin/env bash
# Run as the CI executor owner: cross-user perf cannot read its mappings.
set -euo pipefail
umask 022

executable=$(realpath "$1")
results=$(mktemp -d /tmp/hogd-executor-profile.XXXXXX)
chmod 755 "$results"
echo "EXECUTOR_PROFILE_DIR=$results"
git rev-parse HEAD >"$results/revision.txt"
hostname -s >"$results/host.txt"

# Ignore an executor left over from a prior schedule; the harness removes it.
previous_pids=" $(pgrep -u "$(id -u)" -x hog-exec | tr '\n' ' ' || true) "
deadline=$((SECONDS + 600))
pid=''
while ((SECONDS < deadline)); do
  for candidate in $(pgrep -u "$(id -u)" -x hog-exec || true); do
    [[ "$previous_pids" != *" $candidate "* ]] || continue
    if [[ $(readlink "/proc/$candidate/exe" || true) == "$executable" ]]; then
      pid=$candidate
      break
    fi
  done
  [[ -z "$pid" ]] || break
  sleep 1
done
if [[ -z "$pid" ]]; then
  echo 'EXECUTOR_PROFILE_FAILED: no new executor within 600 seconds'
  exit 1
fi
echo "$pid" >"$results/pid.txt"
cp "$executable" "$results/hog-exec"
sleep 25
date -u >"$results/perf-window.txt"
perf record -F 49 -e cycles:u --call-graph dwarf,4096 -p "$pid" \
  -o "$results/perf.data" -- sleep 60 >"$results/perf-record.log" 2>&1
date -u >>"$results/perf-window.txt"
perf report --stdio --no-children --sort dso,symbol -g none \
  -i "$results/perf.data" >"$results/perf-self.txt" 2>"$results/perf-report-errors.txt"
perf report --stdio --children --sort dso,symbol \
  -i "$results/perf.data" >"$results/perf-children.txt" 2>>"$results/perf-report-errors.txt"
echo "EXECUTOR_PROFILE_READY=$results"
