#!/usr/bin/env bash
# Invoked on the database host by the payment harness, under scheduler locks.
set -Eeuo pipefail

binary=$(readlink -f "${1:?hog-db executable}")
output=${2:?artifact directory}
shift 2
mkdir -p "$output"
exec > >(tee "$output/profile.log") 2>&1

pid=
while read -r candidate; do
  if [[ "$(readlink -f "/proc/$candidate/exe")" == "$binary" ]]; then
    [[ -z "$pid" ]] || { echo "multiple matching hog-db processes"; exit 1; }
    pid=$candidate
  fi
done < <(pgrep -u "$(id -u)" -x hog-db)
[[ -n "$pid" ]] || { echo "no matching hog-db process"; exit 1; }

{
  date -u --iso-8601=seconds
  hostname
  git -C "$(dirname "${BASH_SOURCE[0]}")/../.." rev-parse HEAD
  sha256sum "$binary"
  uname -a
  lscpu
  printf 'pid=%s clock_ticks=%s\n' "$pid" "$(getconf CLK_TCK)"
  tr '\0' ' ' < "/proc/$pid/cmdline"
  printf '\n'
} > "$output/metadata.txt"

snapshot() {
  local label=$1 index=0 url
  shift
  date -u --iso-8601=ns > "$output/$label.time"
  for file in stat status io sched; do
    cat "/proc/$pid/$file" > "$output/$label.$file"
  done
  for url in "$@"; do
    curl --fail --silent --show-error --max-time 5 "$url" \
      > "$output/$label.metrics-$index" || true
    index=$((index + 1))
  done
}

# Allow startup and initialization before a bounded steady-load sample.
# Headline throughput must come from separate runs with profiling disabled.
sleep 15
snapshot before "$@"
perf stat -p "$pid" -e task-clock,cycles,instructions,context-switches,page-faults \
  -o "$output/perf-stat.txt" -- sleep 120 &
stat_pid=$!
status=0
perf record -F 99 -p "$pid" --call-graph dwarf,8192 \
  -o "$output/perf.data" -- sleep 120 || status=$?
wait "$stat_pid" || status=$?
snapshot after "$@"
if [[ "$status" -eq 0 ]]; then
  perf report --stdio --no-children --percent-limit 0.5 \
    -i "$output/perf.data" > "$output/perf-self.txt"
  perf report --stdio --children --percent-limit 1 \
    -i "$output/perf.data" > "$output/perf-callgraph.txt"
  cat "$output/perf-stat.txt"
  head -100 "$output/perf-self.txt"
fi
exit "$status"
