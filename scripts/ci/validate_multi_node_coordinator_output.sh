#!/usr/bin/env bash
# Check each validator independently before concatenating and recording metrics.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 NODE0_STDOUT NODE1_STDOUT" >&2
  exit 2
fi

status=0
node=0
for output in "$@"; do
  if ! awk -F ', ' '
    # A measured nonempty block has a duration/cumulative row followed by its
    # throughput row. Startup-only empty blocks do not demonstrate progress.
    /^Block duration \(s\), / {
      measured = (NF == 8 && $2 ~ /^[0-9]+[.][0-9]+$/ &&
                  $3 == "Num txs in block" && $4 ~ /^[0-9]+$/ && $4 > 0 &&
                  $5 == "Overall duration (s)" && $6 ~ /^[0-9]+[.][0-9]+$/ &&
                  $7 == "Overall number of txs" && $8 ~ /^[0-9]+$/ &&
                  $8 >= $4)
      next
    }
    /^Block throughput \(tx\/s\), / {
      if (measured && NF == 4 && $2 ~ /^[0-9]+[.][0-9]+$/ &&
          $2 > 0 && $3 == "Overall throughput (tx/s)" &&
          $4 ~ /^[0-9]+[.][0-9]+$/ && $4 > 0) usable = 1
    }
    { measured = 0 }
    END { exit !usable }
  ' "$output"; then
    echo "node${node}_chuckd has no usable measured nonempty block in coordinator stdout" >&2
    status=1
  fi
  node=$((node + 1))
done
exit "$status"
