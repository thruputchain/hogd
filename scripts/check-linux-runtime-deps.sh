#!/usr/bin/env bash
# Fails if any binary needs a shared library outside glibc, or a glibc symbol
# version newer than the maximum.
#
# Release binaries link every other library statically (HOGD_STATIC_DEPS), so
# they start on any Linux host with that glibc or newer. Usage:
#   check-linux-runtime-deps.sh <max-glibc-version> <binary>...
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <max-glibc-version, e.g. 2.38> <binary>..." >&2
  exit 1
fi

max="$1"
shift
status=0
glibc_libraries='^(ld-linux[-a-z0-9_]*\.so\.[0-9]+|libc\.so\.6|libm\.so\.6|libpthread\.so\.0|libdl\.so\.2|librt\.so\.1|libutil\.so\.1)$'

for binary in "$@"; do
  if [[ ! -x "$binary" ]]; then
    echo "error: missing executable: $binary" >&2
    status=1
    continue
  fi

  needed=$(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]$/\1/p')
  others=$(grep -Ev "$glibc_libraries" <<<"$needed" || true)
  if [[ -n "$others" ]]; then
    echo "$binary: needs shared libraries outside glibc:" >&2
    sed 's/^/  /' <<<"$others" >&2
    status=1
  fi

  newest=$(objdump -T "$binary" | grep -oE '\bGLIBC_[0-9.]+' | sed 's/^GLIBC_//' | sort -Vu | tail -n 1)
  if [[ -n "$newest" && "$(printf '%s\n' "$max" "$newest" | sort -V | tail -n 1)" != "$max" ]]; then
    echo "$binary: needs glibc $newest, newer than $max" >&2
    status=1
  fi
done

exit "$status"
