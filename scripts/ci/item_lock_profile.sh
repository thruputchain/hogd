#!/usr/bin/env bash
set -u

pid=$1
output=$2
mkdir -p "$output"
if ! command -v perf >/dev/null 2>&1; then
  printf 'perf is not installed\n' >"$output/unavailable.txt"
  exit 0
fi
# Respect the host's existing perf policy; do not change sysctl or permissions.
if ! perf stat -e task-clock -- true 2>"$output/probe.txt"; then
  exit 0
fi
perf stat -e task-clock,context-switches,cpu-migrations,page-faults \
  -p "$pid" -o "$output/stat.txt" -- sleep 15 &
stat_pid=$!
perf record -q -e cpu-clock -F 99 -p "$pid" -o "$output/perf.data" \
  -- sleep 15 2>"$output/record.txt"
record_status=$?
wait "$stat_pid" || true
if [[ $record_status == 0 ]]; then
  perf report --stdio --no-children --percent-limit 0.5 \
    -i "$output/perf.data" >"$output/report.txt" 2>"$output/report.log"
fi
