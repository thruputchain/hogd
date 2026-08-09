#!/bin/sh

set -eu

if [ "$#" -ne 1 ] || [ -z "${HOG_GLIBC_2_43_LOADER:-}" ] \
  || [ -z "${HOG_GLIBC_2_43_LIBDIR:-}" ]; then
  echo "Usage: source prepare_glibc_2_43.sh, then $0 <program>" >&2
  exit 2
fi

program=$1

readelf --program-headers "$program" \
  | grep --fixed-strings --quiet \
    "Requesting program interpreter: ${HOG_GLIBC_2_43_LOADER}"

"$HOG_GLIBC_2_43_LOADER" --list "$program" \
  | grep --fixed-strings --quiet \
    "${HOG_GLIBC_2_43_LIBDIR}/libc.so.6"

echo "Verified glibc ${HOG_GLIBC_2_43_VERSION}: $program"
