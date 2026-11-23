#!/usr/bin/env bash
# Fails if any binary needs a libstdc++ symbol version newer than the maximum.
#
# Release binaries are built with GCC 15 but must run against an older
# libstdc++ (GLIBCXX_3.4.33 is GCC 14's). Usage:
#   check-max-glibcxx.sh <max-version> <binary>...
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <max-version, e.g. 3.4.33> <binary>..." >&2
  exit 1
fi

max="$1"
shift
status=0

for binary in "$@"; do
  if [[ ! -x "$binary" ]]; then
    echo "error: missing executable: $binary" >&2
    status=1
    continue
  fi

  # Dynamic symbol lines end with "GLIBCXX_<version> <symbol>"; keep only those
  # whose version sorts above the maximum.
  newer=$(objdump -T "$binary" | awk -v max="$max" '
    function newer(v, m,    a, b, i, n) {
      n = split(v, a, ".")
      split(m, b, ".")
      for (i = 1; i <= n || i in b; i++) {
        if (a[i] + 0 != b[i] + 0) {
          return a[i] + 0 > b[i] + 0
        }
      }
      return 0
    }
    {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^\(?GLIBCXX_[0-9.]+\)?$/) {
          v = $i
          gsub(/[()]/, "", v)
          sub(/^GLIBCXX_/, "", v)
          if (newer(v, max)) {
            print "GLIBCXX_" v " " $NF
          }
        }
      }
    }
  ' | sort -u)

  if [[ -n "$newer" ]]; then
    echo "$binary: needs libstdc++ newer than GLIBCXX_$max:" >&2
    c++filt <<<"$newer" | sed 's/^/  /' >&2
    status=1
  fi
done

exit "$status"
