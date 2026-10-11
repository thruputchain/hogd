#!/usr/bin/env bash
set -u
set +e
pid=$1
output=$2
trial_log=${3:-}
mkdir -p "$output"
if ! command -v perf >/dev/null 2>&1; then
  echo "perf unavailable" >"$output/status.txt"
  exit 0
fi
# Quiet mode hid errors in the earlier run; retain diagnostics and exit status.
perf record -e cpu-clock -F 99 --clockid mono -p "$pid" -o "$output/perf.data" \
  -- sleep 25 >"$output/record.out" 2>"$output/record.txt"
record_status=$?
echo "record_status=$record_status" >"$output/status.txt"
if [[ -s $output/perf.data ]]; then
  perf report --stdio --no-children --percent-limit 0.5 \
    -i "$output/perf.data" >"$output/report.txt" 2>"$output/report.log"
  echo "report_status=$?" >>"$output/status.txt"
  perf script -i "$output/perf.data" -F time,sym,dso 2>"$output/script.log" |
    python3 scripts/ci/item_lock_samples.py "$trial_log" >"$output/samples.json"
  echo "script_status=$?" >>"$output/status.txt"
fi
exit 0
