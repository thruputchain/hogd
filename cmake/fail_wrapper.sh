#!/bin/bash

out=$(mktemp)
err=$(mktemp)

FAIL_LOG="$1"

# Ensure the directory exists
mkdir -p "$(dirname "$FAIL_LOG")"

# Note: We don't create the file here - let it be created on first failure
# This preserves true append behavior

# Run the command, streaming output and also capturing
"${@:2}" > >(tee "$out") 2> >(tee "$err" >&2)
result=$?

if [[ $result -ne 0 ]]; then
  {
    echo '======================================'
    echo 'FAILED COMMAND:'
    echo "  ${@:2}"
    echo '--------------------------------------'
    if [[ -s "$out" ]]; then
        echo 'STDOUT:'
        cat "$out"
        echo '--------------------------------------'
    fi
    if [[ -s "$err" ]]; then
        echo 'STDERR:'
        cat "$err"
        echo '--------------------------------------'
    fi
    echo "Exit code: $result"
    echo '======================================'
  } >> "$FAIL_LOG"
fi

rm -f "$out" "$err"
exit $result
