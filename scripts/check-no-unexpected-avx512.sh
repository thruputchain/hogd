#!/usr/bin/env bash
set -euo pipefail

if [[ $# -eq 0 ]]; then
  echo "usage: $0 <binary>..." >&2
  exit 1
fi

status=0

for binary in "$@"; do
  if [[ ! -x "$binary" ]]; then
    echo "error: missing executable: $binary" >&2
    status=1
    continue
  fi

  # BLAKE3 includes explicit runtime-dispatched AVX-512 implementations. Those
  # symbols are allowed; generic dependency or application code is not.
  if objdump -d --demangle --no-show-raw-insn "$binary" | awk -v binary="$binary" '
    /^[[:xdigit:]]+ <.*>:$/ {
      symbol = $0
      sub(/^[[:xdigit:]]+ </, "", symbol)
      sub(/>:$/, "", symbol)
      next
    }
    {
      has_avx512 = \
        $0 ~ /%zmm[0-9]+/ || \
        $0 ~ /(^|[^[:alnum:]_])zmm[0-9]+([^[:alnum:]_]|$)/ || \
        $0 ~ /%k[1-7]([^[:digit:]]|$)/ || \
        $0 ~ /(^|[^[:alnum:]_])k[1-7]([^[:alnum:]_]|$)/
    }
    has_avx512 {
      if (symbol ~ /^_?blake3_.*_avx512$/) {
        next
      }
      found++
      if (found <= 25) {
        printf("%s: unexpected AVX-512 in %s: %s\n", binary, symbol, $0)
      }
    }
    END {
      if (found > 25) {
        printf("%s: ... %d more unexpected AVX-512 instructions omitted\n",
               binary, found - 25)
      }
      exit found ? 1 : 0
    }
  '; then
    :
  else
    status=1
  fi
done

exit "$status"
