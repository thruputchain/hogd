#!/usr/bin/env bash
set -u
set +e
set -o pipefail
pid=$1
output=$2
trial_log=${3:-}
mkdir -p "$output"
if ! command -v perf >/dev/null 2>&1; then
  echo "perf unavailable" >"$output/status.txt"
  exit 0
fi
# Each existing thread gets one event, rather than one event per CPU.
# The 2048-worker pool is already alive and remains alive through these trials.
date -u +%FT%TZ >"$output/start.txt"
perf record --timestamp --per-thread --no-inherit -m 4 -e cpu-clock -F 99 \
  --call-graph dwarf,2048 --clockid mono -p "$pid" -o "$output/perf.data" \
  -- sleep 12 >"$output/record.out" 2>"$output/record.txt"
record_status=$?
date -u +%FT%TZ >"$output/end.txt"
echo "record_status=$record_status" >"$output/status.txt"
if [[ -s $output/perf.data ]]; then
  perf report --stdio --no-children --call-graph none --percent-limit 0.5 \
    -i "$output/perf.data" >"$output/report.txt" 2>"$output/report.log"
  echo "report_status=$?" >>"$output/status.txt"
  perf script -i "$output/perf.data" -F time,ip,sym,dso 2>"$output/script.log" |
    python3 scripts/ci/item_lock_samples.py "$trial_log" >"$output/samples.json"
  echo "script_status=$?" >>"$output/status.txt"
fi
exit 0
