#!/bin/bash
# Phase 3a: a real, measured slow tier without CXL or NVM hardware.
#
# The loglet heap is backed by a file on NVMe and the process runs under a
# MemoryMax cap. Pages the kernel keeps resident are the fast tier; evicted ones
# come back from the device on a major fault. Linux LRU reclaim becomes the
# underlying page-based tiering system, which is exactly the arrangement
# ObjecTier is designed to sit on top of rather than replace.
#
# The number this exists for is the major-fault count, scattered vs
# consolidated: a direct measurement of slow-tier accesses that assumes none of
# the latency or capacity constants the modelled AMAT depends on.
#
# One run with -C produces both arms -- the bench replays the same workload
# after consolidating -- so cap, accesses and page-cache history are identical
# between them.
set -u

BIN=${BIN:-/home/hog12/vinita/hogd-obj/build/hogd/hogdb/hogdb_bench}
# Heap on the NVMe root fs. The WAL stays on tmpfs so log writes do not contend
# with paging traffic on the same device queue.
HEAP_FILE=${HEAP_FILE:-/var/tmp/objtier-heap.bin}
DB=${DB:-/tmp/claude-1024/-home-hog12-vinita-hogd-obj/bd4e7a3e-6b26-4483-9baa-589b20792b20/scratchpad/objdb_ssd}
OUT=${OUT:-/home/hog12/vinita/hogd-obj/docs/objectier/data}

NUM=${NUM:-500000}
W=${W:-8}
THETA=${THETA:-0.99}
HEAP=${HEAP:-1.5}
# Absolute resident cap. Must exceed the process's anonymous footprint (ops
# vector, hit maps, sketch), which is not reclaimable without swap -- set it too
# low and the run is OOM-killed rather than paged. Whatever is left above that
# footprint is what the heap keeps resident, i.e. the fast tier.
CAP=${CAP:-1200M}

mkdir -p "$DB" "$OUT" "$(dirname "$HEAP_FILE")"
trap 'rm -f "$HEAP_FILE"' EXIT

run() {
  rm -rf "$DB"; mkdir -p "$DB"; rm -f "$HEAP_FILE"
  RAMDB_HEAP_FILE="$HEAP_FILE" systemd-run --user --scope --quiet $1 -- \
    timeout 1800 "$BIN" "$DB" zipf_payments \
      -n "$NUM" -w "$W" -t "$THETA" -H "$HEAP" -C 2>/dev/null
}

echo "=== uncapped reference (whole heap resident) ===" >&2
UNCAPPED=$(run "")
HEAP_BYTES=$(stat -c %s "$HEAP_FILE" 2>/dev/null || echo 0)
echo "$UNCAPPED" | grep -E '^faults,|^layout,' >&2
echo "heap file: $HEAP_BYTES bytes" >&2

echo "=== capped at $CAP ===" >&2
CAPPED=$(run "-p MemoryMax=$CAP -p MemorySwapMax=0")
echo "$CAPPED" | grep -E '^faults,|^layout,' >&2

emit() {
  while IFS= read -r f; do
    arm=${f#faults,}; arm=${arm%%,*}
    hit=$(echo "$2" | grep "^layout,$arm," | cut -d, -f6)
    echo "$arm,$1,$HEAP_BYTES,$(echo "$f" | cut -d, -f3-5),${hit:-}"
  done < <(echo "$2" | grep '^faults,')
}

{
  echo "arm,cap,heap_bytes,major_faults,minor_faults,seconds,hit_rate"
  emit uncapped "$UNCAPPED"
  emit "$CAP" "$CAPPED"
} > "$OUT/ssd_tier.csv"

column -t -s, "$OUT/ssd_tier.csv" >&2
