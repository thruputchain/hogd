#!/bin/bash
# X3: item-size sweep. Tests the ObjecTier paper's untested Section 4.3
# prediction that consolidation helps more as objects get smaller, because more
# of them share a tiering unit.
#
# Item count is held constant so the rank population, the skew, and the drawn
# access sequence are identical at every point -- the only variable is how many
# items fit in a 2 MB loglet. Hot-byte utilization is a per-loglet fraction, so
# it stays comparable even as the working set grows.
#
# Capped at 32 of 128 threads: the GitHub Actions runner on this host is live,
# and a CI job landing mid-sweep should still find the box usable.
set -u

BIN=${BIN:-/home/hog12/vinita/hogd-obj/build/hogd/ramdb/ramdb_bench}
DB=${DB:-/tmp/claude-1024/-home-hog12-vinita-hogd-obj/bd4e7a3e-6b26-4483-9baa-589b20792b20/scratchpad/objdb_x3}
OUT=${OUT:-/home/hog12/vinita/hogd-obj/docs/objectier/data/x3_size_sweep.csv}
NUM=${NUM:-25000000}
THREADS=${THREADS:-32}
THETA=${THETA:-0.99}
MULT=${MULT:-3}
SIZES=${SIZES:-"16 32 64 128 256 512"}

mkdir -p "$DB" "$(dirname "$OUT")"
echo "value_size,layout,loglets,items,accesses,mean_hot,mean_live,fast_hot,hit,amat_ns" > "$OUT"

for vsz in $SIZES; do
  for layout in scattered consolidated; do
    echo "[$(date +%H:%M:%S)] value_size=$vsz layout=$layout" >&2
    raw=$("$BIN" "$DB" "layout_$layout" "$NUM" "$vsz" "$THETA" "$THREADS" "$MULT" 2>/dev/null)
    s=$(echo "$raw" | grep '^layout_summary,' | head -1)
    a=$(echo "$raw" | grep '^layout_amat,'    | head -1)
    if [ -z "$s" ] || [ -z "$a" ]; then
      echo "[$(date +%H:%M:%S)] FAILED at value_size=$vsz layout=$layout" >&2
      continue
    fi
    # layout_summary,label,loglets,items,accesses,mean_hot,mean_live,fast_hot
    # layout_amat,label,fast_frac,fast_count,hit,amat_ns
    echo "$vsz,$layout,$(echo "$s" | cut -d, -f3-8),$(echo "$a" | cut -d, -f5-6)" >> "$OUT"
    # Keep the per-run CDF so a size point can be plotted on its own later.
    echo "$raw" | grep '^layout_cdf,' \
      > "$(dirname "$OUT")/x3_cdf_${vsz}_${layout}.csv"
  done
done

echo "[$(date +%H:%M:%S)] done -> $OUT" >&2
column -t -s, "$OUT" >&2
