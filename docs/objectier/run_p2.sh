#!/bin/bash
# Capture the trigger and aging results to CSV, for plot_p2.py and plot_p1.py.
#
# Three experiments, all through the real hogdb storage path:
#
#   p2_convergence  the layout under a triggered pass, block by block, until it
#                   converges and the trigger stops selecting anything.
#   p1_aging        the same with the hot set re-drawn mid-run, swept over the
#                   sketch's aging epoch. Without aging the trigger goes silent
#                   and the layout stays broken; the epoch is what recovers it.
#   p1_threshold    the hot-set threshold the sketch derives for a requested
#                   share of tracked keys, and what that share is worth.
set -u

BIN=${BIN:-/home/hog12/vinita/hogd-obj/build/hogd/hogdb/hogdb_bench}
OUT=${OUT:-$(dirname "$0")/data}
DB=${DB:-/tmp/objtier-p2-db}

NUM=${NUM:-500000}
THETA=${THETA:-0.99}
W=${W:-22}          # sketch width order: 2^22 counters/row, collision-free here
FRAC=${FRAC:-0.20}  # hot-set share, matching the modelled fast tier
VICTIMS=${VICTIMS:-2}

mkdir -p "$OUT"

# run <extra flags...> -- emits the per-block "block,"/"layout,auto" pairs.
run() {
  rm -rf "$DB"; mkdir -p "$DB"
  timeout 3600 "$BIN" "$DB" zipf_payments \
    -n "$NUM" -t "$THETA" -W "$W" -F "$FRAC" -A "$VICTIMS" "$@" 2>/dev/null
}

# join_blocks pairs each "block," line with the "layout,auto<n>," that follows
# it, since the two carry different halves of one block's result.
join_blocks() {
  paste - - < <(grep -aE '^(block,|layout,auto)' ) |
    awk -F'\t' -v pfx="$1" '{
      split($1, b, ","); split($2, l, ",");
      print pfx b[2] "," b[5] "," b[6] "," l[5] "," l[6] "," l[7];
    }'
}

echo "=== p2: convergence and termination ===" >&2
{
  echo "block,loglets_moved,consolidate_seconds,fast_hot_bytes,hit_rate,amat_ns"
  # 24 blocks, not 16: the trigger converges by 13, and the point of the figure
  # is the stretch afterwards where it keeps selecting nothing.
  run -r 24 | join_blocks ""
} > "$OUT/p2_convergence.csv"
column -t -s, "$OUT/p2_convergence.csv" >&2

echo "=== p1: aging epoch sweep, hot set re-drawn every 15 blocks ===" >&2
{
  echo "age_epoch,block,loglets_moved,consolidate_seconds,fast_hot_bytes,hit_rate,amat_ns"
  # 0 = never aged. The sketch then records "was ever hot", which is the case
  # every other workload here exercises by accident.
  run -r 40 -E 15            | join_blocks "0,"
  for EPOCH in 20 10 5 2; do
    run -r 40 -E 15 -G "$EPOCH" | join_blocks "$EPOCH,"
  done
} > "$OUT/p1_aging.csv"
awk -F, 'NR==1||$2==13||$2==29||$2==39' "$OUT/p1_aging.csv" |
  column -t -s, >&2

echo "=== p1: derived threshold vs requested hot-set share ===" >&2
{
  echo "hot_fraction,derived_threshold,hit_rate,amat_ns"
  for F in 0.02 0.05 0.10 0.20 0.30 0.40; do
    rm -rf "$DB"; mkdir -p "$DB"
    OUTPUT=$(timeout 1800 "$BIN" "$DB" zipf_payments -n "$NUM" -t "$THETA" \
               -r 3 -W "$W" -F "$F" -C 2>/dev/null)
    THRESH=$(echo "$OUTPUT" | grep -a '^consolidated,' | cut -d, -f4)
    LAYOUT=$(echo "$OUTPUT" | grep -a '^layout,zipf_payments+consolidated')
    echo "$F,$THRESH,$(echo "$LAYOUT" | cut -d, -f6),$(echo "$LAYOUT" | cut -d, -f7)"
  done
} > "$OUT/p1_threshold.csv"
column -t -s, "$OUT/p1_threshold.csv" >&2

rm -rf "$DB"
