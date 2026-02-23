#!/bin/bash
# Wraps test execution so that on timeout, a stack trace is captured via gdb
# before the process is killed.  This provides post-mortem diagnostics without
# relying on core dumps (which CI runners may intercept).
#
# Usage: set CROSSCOMPILING_EMULATOR on test targets to this script.
# Override the timeout with CI_TEST_TIMEOUT_SECS (default 90).
# Backtraces are written to CI_BACKTRACE_DIR (default /tmp/backtraces).

# During test discovery (gtest_discover_tests runs --gtest_list_tests at build
# time), pass through directly — the background approach breaks stdout capture.
for arg in "$@"; do
  if [ "$arg" = "--gtest_list_tests" ]; then
    exec "$@"
  fi
done

TIMEOUT=${CI_TEST_TIMEOUT_SECS:-90}
BT_DIR=${CI_BACKTRACE_DIR:-/tmp/backtraces}
mkdir -p "$BT_DIR"

# Run the test and a sleep timer in the background.  sleep is a direct
# child (not inside a subshell), so `kill $SLEEP_PID` targets the sleep
# process itself — no bash signal-deferral issues.
"$@" &
TEST_PID=$!

sleep "$TIMEOUT" &
SLEEP_PID=$!

# Wait for whichever finishes first (bash 4.3+).
wait -n "$TEST_PID" "$SLEEP_PID" 2>/dev/null

if kill -0 "$TEST_PID" 2>/dev/null; then
  # Test still running => timeout fired.  Capture backtrace, then kill.
  BT_FILE="$BT_DIR/bt.$(basename "$1").$TEST_PID"
  {
    echo "=== TIMEOUT (${TIMEOUT}s) ==="
    echo "CMD: $*"
    echo "PID: $TEST_PID"
    date -Iseconds
    echo "---"
    if command -v gdb >/dev/null 2>&1; then
      gdb -batch -ex 'thread apply all bt' -p "$TEST_PID" 2>&1
    else
      echo "(gdb not installed — no backtrace available)"
    fi
  } >"$BT_FILE" 2>&1
  echo "timeout: backtrace saved to $BT_FILE" >&2

  kill -KILL "$TEST_PID" 2>/dev/null
  wait "$TEST_PID" 2>/dev/null
  kill "$SLEEP_PID" 2>/dev/null
  wait "$SLEEP_PID" 2>/dev/null
  exit 124
else
  # Test finished before timeout.  Clean up the sleep.
  wait "$TEST_PID" 2>/dev/null
  TEST_EXIT=$?
  kill "$SLEEP_PID" 2>/dev/null
  wait "$SLEEP_PID" 2>/dev/null
  exit "$TEST_EXIT"
fi
