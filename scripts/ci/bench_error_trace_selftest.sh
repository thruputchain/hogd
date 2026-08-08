#!/usr/bin/env bash
# Self-test for the error-trace helpers in bench_harness.sh: the ERR trap must
# name the command that aborts a benchmark script, stay quiet where a benchmark
# script deliberately tolerates a failure, and leave a digest the schedule can
# replay at the end of the job.
set -euo pipefail

HARNESS="$(dirname "$(readlink -f "$0")")/bench_harness.sh"
WORK_DIR=$(mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT
fail=0

# Run a script body with the harness sourced. Prints the body's combined output;
# returns the body's exit status.
run_case() {
  local body=$1
  local script="$WORK_DIR/case.sh"

  cat >"$script" <<EOF
#!/usr/bin/env bash
set -Eeuo pipefail
. "$HARNESS"
$body
EOF
  CI_BENCH_FAILURE_LOG="$WORK_DIR/failures.log" bash "$script" 2>&1
}

expect_status() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: exit status $got, want $want"
    fail=1
  else
    echo "ok   $label status"
  fi
}

expect_contains() {
  local label=$1 needle=$2 haystack=$3
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL $label: output missing [$needle]"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

expect_lacks() {
  local label=$1 needle=$2 haystack=$3
  if [[ "$haystack" == *"$needle"* ]]; then
    echo "FAIL $label: output should not contain [$needle]"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

# A command that ends the script is named, with its status and location.
out=$(run_case '
ci_enable_error_trace
wait_for_thing() { return 7; }
launch() { wait_for_thing; }
launch
echo "NOT REACHED"
') && status=0 || status=$?
expect_status 'aborting command' 7 "$status"
expect_contains 'aborting command reports status' 'exited with status 7' "$out"
expect_contains 'aborting command is named' 'command: return 7' "$out"
expect_contains 'call stack names the caller' 'in launch' "$out"
expect_lacks 'script stops at the failure' 'NOT REACHED' "$out"

# The digest holds one line per aborted script, for replay at the end of the job.
digest=$(cat "$WORK_DIR/failures.log")
expect_contains 'digest records the status' 'exit status 7' "$digest"
expect_contains 'digest records the command' 'return 7' "$digest"

report=$(CI_BENCH_FAILURE_LOG="$WORK_DIR/failures.log" bash -c \
  ". '$HARNESS'; ci_report_bench_failures")
expect_contains 'digest replay includes the failure' 'exit status 7' "$report"

# errtrace puts the trap in the subshell bash forks for a pipeline component, so
# a function failing in a pipeline reports there and the parent shell then
# reports the pipeline itself, naming its last command. Only the first, most
# specific report survives.
rm -f "$WORK_DIR"/failures.log*
out=$(run_case '
ci_enable_error_trace
produce() { /bin/false; }
produce | cat
echo "NOT REACHED"
') && status=0 || status=$?
expect_status 'failure in a pipeline' 1 "$status"
expect_contains 'pipeline names the failing command' 'command: /bin/false' "$out"
expect_lacks 'pipeline does not name its last command' 'command: cat' "$out"
reports=$(grep -c '^FAILED:' <<<"$out" || true)
if [[ "$reports" != 1 ]]; then
  echo "FAIL pipeline failure reported $reports times, want 1"
  printf '%s\n' "$out" | sed 's/^/     | /'
  fail=1
else
  echo 'ok   pipeline failure reported once'
fi
digest_lines=$(wc -l <"$WORK_DIR/failures.log")
if [[ "$digest_lines" != 1 ]]; then
  echo "FAIL pipeline failure wrote $digest_lines digest lines, want 1"
  sed 's/^/     | /' "$WORK_DIR/failures.log"
  fail=1
else
  echo 'ok   pipeline failure writes one digest line'
fi

# Suppressing propagation must not suppress a later script's failure: the
# combined-metrics job runs five suites against one digest.
rm -f "$WORK_DIR"/failures.log*
run_case '
ci_enable_error_trace
/bin/false
' >/dev/null 2>&1 || true
run_case '
ci_enable_error_trace
exit 9
' >/dev/null 2>&1 || true
run_case '
ci_enable_error_trace
/bin/false
' >/dev/null 2>&1 || true
digest_lines=$(wc -l <"$WORK_DIR/failures.log")
if [[ "$digest_lines" != 2 ]]; then
  echo "FAIL three scripts, two aborting: $digest_lines digest lines, want 2"
  sed 's/^/     | /' "$WORK_DIR/failures.log"
  fail=1
else
  echo 'ok   a later script is not suppressed by an earlier report'
fi

# The ERR trap fires whether or not errexit is set. A benchmark script that
# deliberately tolerates a status -- mempool_bench.sh treats the timeout that
# ends the mempool run as success -- must not be reported as aborting.
rm -f "$WORK_DIR"/failures.log*
out=$(run_case '
ci_enable_error_trace
set +e
(exit 124)
tolerated=$?
set -e
echo "tolerated=$tolerated"
false || true
if (exit 3); then :; fi
echo "reached the end"
') && status=0 || status=$?
expect_status 'tolerated failures' 0 "$status"
expect_contains 'tolerated failure runs on' 'tolerated=124' "$out"
expect_contains 'tolerated failure reaches the end' 'reached the end' "$out"
expect_lacks 'tolerated failure is not reported' 'FAILED:' "$out"
if [[ -e "$WORK_DIR/failures.log" ]]; then
  echo 'FAIL tolerated failure wrote a digest entry'
  fail=1
else
  echo 'ok   tolerated failure leaves no digest'
fi

# Cleanup handlers run after ci_enter_cleanup, whose failures are expected.
rm -f "$WORK_DIR"/failures.log*
out=$(run_case '
ci_enable_error_trace
cleanup() {
  ci_enter_cleanup
  false
  echo "cleanup finished"
}
trap "cleanup" EXIT
echo "body done"
') && status=0 || status=$?
expect_status 'cleanup handler' 0 "$status"
expect_contains 'cleanup handler runs to the end' 'cleanup finished' "$out"
expect_lacks 'cleanup failures are not reported' 'FAILED:' "$out"

# A successful script reports nothing and leaves no digest.
rm -f "$WORK_DIR"/failures.log*
out=$(run_case '
ci_enable_error_trace
echo "all good"
') && status=0 || status=$?
expect_status 'successful script' 0 "$status"
expect_lacks 'successful script is silent' 'FAILED:' "$out"

report=$(CI_BENCH_FAILURE_LOG="$WORK_DIR/failures.log" bash -c \
  ". '$HARNESS'; ci_report_bench_failures")
if [[ -n "$report" ]]; then
  echo "FAIL empty digest should print nothing, got [$report]"
  fail=1
else
  echo 'ok   empty digest prints nothing'
fi

# A digest that cannot be written must not mask the benchmark failure.
out=$(CI_BENCH_FAILURE_LOG=/proc/nonexistent/failures.log bash -c "
set -Eeuo pipefail
. '$HARNESS'
ci_enable_error_trace
false
" 2>&1) && status=0 || status=$?
expect_status 'unwritable digest' 1 "$status"
expect_contains 'unwritable digest still reports' 'exited with status 1' "$out"

# ci_reset_bench_failures clears a digest left by an earlier run.
printf 'stale entry\n' >"$WORK_DIR/failures.log"
CI_BENCH_FAILURE_LOG="$WORK_DIR/failures.log" bash -c \
  ". '$HARNESS'; ci_reset_bench_failures"
if [[ -e "$WORK_DIR/failures.log" ]]; then
  echo 'FAIL ci_reset_bench_failures left the stale digest in place'
  fail=1
else
  echo 'ok   ci_reset_bench_failures clears a stale digest'
fi

exit "$fail"
