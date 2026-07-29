#!/bin/bash
# V1 sweeps: (a) AMAT vs skew, before and after consolidation, against the
# oracle-sorted ceiling; (b) benefit vs work done, sweeping the mixedness
# threshold that selects victims.
set -u

BIN=${BIN:-/home/hog12/vinita/hogd-obj/build/hogd/hogdb/hogdb_bench}
DB=${DB:-/tmp/claude-1024/-home-hog12-vinita-hogd-obj/bd4e7a3e-6b26-4483-9baa-589b20792b20/scratchpad/objdb_v1sweep}
OUT=${OUT:-/home/hog12/vinita/hogd-obj/docs/objectier/data}
NUM=${NUM:-2000000}
W=${W:-32}

mkdir -p "$DB" "$OUT"

# Extract the trailing "layout,<label>,<loglets>,<fast>,<fast_hot>,<hit>,<amat>"
# for a given label from a run's output.
field() { echo "$1" | grep "^layout,$2," | tail -1 | cut -d, -f"$3"; }

echo "theta,arm,hit,amat_ns" > "$OUT/v1_skew.csv"
for th in 0 0.3 0.6 0.8 0.9 0.99; do
  rm -rf "$DB"; mkdir -p "$DB"
  out=$("$BIN" "$DB" zipf_payments -n "$NUM" -w "$W" -t "$th" -C 2>/dev/null)
  echo "$th,scattered,$(field "$out" zipf_payments 6),$(field "$out" zipf_payments 7)" >> "$OUT/v1_skew.csv"
  echo "$th,consolidated,$(field "$out" 'zipf_payments+consolidated' 6),$(field "$out" 'zipf_payments+consolidated' 7)" >> "$OUT/v1_skew.csv"

  rm -rf "$DB"; mkdir -p "$DB"
  out=$("$BIN" "$DB" zipf_payments -n "$NUM" -w "$W" -t "$th" -S 2>/dev/null)
  echo "$th,ceiling,$(field "$out" zipf_payments 6),$(field "$out" zipf_payments 7)" >> "$OUT/v1_skew.csv"
  echo "[$(date +%H:%M:%S)] skew theta=$th done" >&2
done

echo "min_mixedness,loglets_moved,hit,amat_ns" > "$OUT/v1_mixedness.csv"
for mx in 0.0 0.005 0.010 0.015 0.020 0.025 0.030 0.040; do
  rm -rf "$DB"; mkdir -p "$DB"
  out=$("$BIN" "$DB" zipf_payments -n "$NUM" -w "$W" -t 0.99 -C -m "$mx" 2>/dev/null)
  moved=$(echo "$out" | grep '^consolidated,' | tail -1 | cut -d, -f2)
  echo "$mx,${moved:-0},$(field "$out" 'zipf_payments+consolidated' 6),$(field "$out" 'zipf_payments+consolidated' 7)" >> "$OUT/v1_mixedness.csv"
  echo "[$(date +%H:%M:%S)] mixedness=$mx moved=${moved:-0}" >&2
done

echo "[$(date +%H:%M:%S)] done" >&2
column -t -s, "$OUT/v1_skew.csv" >&2
column -t -s, "$OUT/v1_mixedness.csv" >&2
