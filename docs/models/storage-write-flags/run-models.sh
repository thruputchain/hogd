#!/bin/sh
set -eu
cd "$(dirname "$0")"
JAVA_BIN=${JAVA_BIN:-java}
: "${TLC_JAR:?Set TLC_JAR to an absolute path to tla2tools.jar}"
for policy in blind nnint fresh stale stickyall lock; do
  status=0
  "$JAVA_BIN" -Xmx2g -XX:+UseParallelGC -cp "$TLC_JAR" tlc2.TLC \
    -workers 1 -fp 0 -seed 0 -config "$policy.cfg" FlagReadback.tla \
    > "$policy.log" 2>&1 || status=$?
  case "$policy" in
    fresh|lock)
      test "$status" -eq 0
      grep -q 'No error has been found' "$policy.log"
      ;;
    stickyall)
      test "$status" -eq 12
      grep -q 'Invariant OtherUsesLastWrite is violated' "$policy.log"
      ;;
    *)
      test "$status" -eq 12
      grep -q 'Invariant ExclusiveRetained is violated' "$policy.log"
      ;;
  esac
  printf '%s: expected result verified\n' "$policy"
done
