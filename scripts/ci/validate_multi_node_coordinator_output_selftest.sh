#!/usr/bin/env bash
set -euo pipefail
validator="$(dirname "$0")/validate_multi_node_coordinator_output.sh"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat >"$tmp/empty_blocks" <<'DATA'
Block duration (s), 0.10, Num txs in block, 0, Overall duration (s), 0.50, Overall number of txs, 0
Block throughput (tx/s), 0.00, Overall throughput (tx/s), 0.00
DATA
cat >"$tmp/measured" <<'DATA'
Block duration (s), 0.15, Num txs in block, 1, Overall duration (s), 0.65, Overall number of txs, 1
Block throughput (tx/s), 6.67, Overall throughput (tx/s), 1.54
DATA
touch "$tmp/empty"
head -1 "$tmp/measured" >"$tmp/partial"
cat "$tmp/empty_blocks" "$tmp/measured" >"$tmp/short"
"$validator" "$tmp/short" "$tmp/measured"
for invalid in empty empty_blocks partial; do
  if "$validator" "$tmp/$invalid" "$tmp/measured"; then
    echo "incorrectly accepted $invalid node0" >&2
    exit 1
  fi
  if "$validator" "$tmp/measured" "$tmp/$invalid"; then
    echo "incorrectly accepted $invalid node1" >&2
    exit 1
  fi
done
if "$validator" "$tmp/empty" "$tmp/empty"; then
  echo "incorrectly accepted two empty outputs" >&2
  exit 1
fi
echo 'multi-node coordinator validation self-test passed'
