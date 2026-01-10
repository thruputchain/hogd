#!/bin/bash

# Usage: scripts/filter_tidy_log.sh <tidy-log-file>
# Reads the provided log file and filters out routine tidy noise,
# mirroring the filtering done in check.sh.

set -euo pipefail

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <tidy-log-file>" >&2
  exit 1
fi

LOG_FILE="$1"

if [ ! -f "$LOG_FILE" ]; then
  echo "error: file not found: $LOG_FILE" >&2
  exit 1
fi

cat "$LOG_FILE" |
  grep -v "^[0-9]\+ warnings generated\.$" |
  grep -v "^\[[[:space:]]*[0-9]\+/[0-9]\+\]" |
  grep -v "^[[:space:]]*Checking" |
  grep -v "^[[:space:]]*Compiling" |
  grep -v "^[[:space:]]*Finished" |
  grep -v "^/usr/bin/clang-tidy-" |
  grep -v "^$"
