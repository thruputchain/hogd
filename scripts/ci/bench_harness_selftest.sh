#!/usr/bin/env bash
# Self-test for the step-capture helpers in bench_harness.sh: exit-status
# rendering, per-step log capture, and the end-of-job failure report that
# replays a failed step's log tail.
set -euo pipefail

. "$(dirname "$(readlink -f "$0")")/bench_harness.sh"

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

CI_BENCH_LOG_DIR="$work_dir/bench_logs"
fail=0

check_eq() {
  local label="$1" want="$2" got="$3"
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: got [$got] want [$want]"
    fail=1
  else
    echo "ok   $label"
  fi
}

check_contains() {
  local label="$1" needle="$2" haystack="$3"
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL $label: [$needle] not found in:"
    echo "$haystack"
    fail=1
  else
    echo "ok   $label"
  fi
}

## ci_describe_exit_status

check_eq "describe 1" "exit status 1" "$(ci_describe_exit_status 1)"
check_eq "describe 124" "exit status 124 (timed out)" \
  "$(ci_describe_exit_status 124)"
# 134 is the status this job reports when hog-mempool aborts, and the signal
# name is the whole point of the helper.
check_eq "describe 134" "exit status 134 (killed by signal 6: SIGABRT)" \
  "$(ci_describe_exit_status 134)"
check_eq "describe 139" "exit status 139 (killed by signal 11: SIGSEGV)" \
  "$(ci_describe_exit_status 139)"
check_eq "describe 143" "exit status 143 (killed by signal 15: SIGTERM)" \
  "$(ci_describe_exit_status 143)"

## ci_run_bench_step

CI_FAILED_BENCH_STEPS=()
ci_run_bench_step passing bash -c 'echo hello from passing step'
check_eq "passing step records no failure" "0" "${#CI_FAILED_BENCH_STEPS[@]}"
check_eq "passing step captured output" "hello from passing step" \
  "$(cat "$CI_BENCH_LOG_DIR/passing.log")"

# A failing step must not abort the caller even under [set -e], so that the
# remaining suites still run and every failure is reported together.
ci_run_bench_step failing bash -c 'echo step stderr >&2; exit 3'
check_eq "failing step recorded" "failing:3" "${CI_FAILED_BENCH_STEPS[0]}"
check_contains "failing step captured stderr" "step stderr" \
  "$(cat "$CI_BENCH_LOG_DIR/failing.log")"

# The step's own status wins over tee's, which always succeeds.
CI_FAILED_BENCH_STEPS=()
ci_run_bench_step aborting bash -c 'echo about to abort; kill -ABRT $$'
check_eq "signal status taken from step, not tee" "aborting:134" \
  "${CI_FAILED_BENCH_STEPS[0]}"

## ci_report_failed_bench_steps

CI_FAILED_BENCH_STEPS=()
ci_run_bench_step green bash -c 'true'
if ci_report_failed_bench_steps >/dev/null; then
  echo "ok   report succeeds when every step passed"
else
  echo "FAIL report should succeed when every step passed"
  fail=1
fi

CI_FAILED_BENCH_STEPS=()
ci_run_bench_step red bash -c 'echo needle-in-the-log; exit 134' >/dev/null
report=$(ci_report_failed_bench_steps) && {
  echo "FAIL report should fail when a step failed"
  fail=1
}
check_contains "report names the failed step and signal" \
  "FAILED benchmark step 'red': exit status 134 (killed by signal 6: SIGABRT)" \
  "$report"
check_contains "report replays the failed step's log" "needle-in-the-log" \
  "$report"

# A step whose log went missing must still be named rather than silently
# dropping the only record of the failure.
CI_FAILED_BENCH_STEPS=("ghost:1")
report=$(ci_report_failed_bench_steps) || true
check_contains "report tolerates a missing log" "(no captured log at" "$report"

# The replay must not itself be traced, or the log tail is unreadable.
CI_FAILED_BENCH_STEPS=()
ci_run_bench_step traced bash -c 'echo traced-needle; exit 1' >/dev/null 2>&1
set -x
report=$(ci_report_failed_bench_steps 2>/dev/null) || true
case $- in
*x*) echo "ok   xtrace restored after report" ;;
*)
  echo "FAIL xtrace not restored after report"
  fail=1
  ;;
esac
set +x
check_contains "traced replay still readable" "traced-needle" "$report"

## ci_wait_port

# A wait that times out has to name itself in the digest. The suite it aborts
# dies several frames up at a bare [return 1], so this line is the only thing
# in the surviving end-of-job report that says which port never came up.
CI_BENCH_FAILURE_LOG="$work_dir/failures.log"
rm -f "$CI_BENCH_FAILURE_LOG"
# Port 1 is privileged and nothing in CI binds it, so the wait is guaranteed to
# reach its timeout rather than racing a real listener.
if ci_wait_port 127.0.0.1 1 1 >/dev/null 2>&1; then
  echo "FAIL wait on a dead port should fail"
  fail=1
else
  echo "ok   wait on a dead port fails"
fi
check_contains "timed-out wait names host and port in the digest" \
  "nothing listening on 127.0.0.1:1 after 1s" "$(cat "$CI_BENCH_FAILURE_LOG")"
check_contains "timed-out wait names the suite in the digest" \
  "bench_harness_selftest.sh" "$(cat "$CI_BENCH_FAILURE_LOG")"

# A wait that is satisfied leaves the digest alone -- the combined-metrics job
# runs hundreds of them against one digest, and a note from a wait that came up
# would read as this run's failure.
#
# The fixture binds port 0 and reports back whichever port the kernel gave it,
# rather than guessing one: a guessed port can already be taken, and then the
# wait is answered by a listener the test knows nothing about -- a pass that
# means nothing, or a timeout blamed on the helper. It prints only once the
# socket is listening, so the wait below cannot race the fixture's startup.
rm -f "$CI_BENCH_FAILURE_LOG"
listener_out="$work_dir/listener_port"
: >"$listener_out"
python3 -c '
import socket, sys, time

s = socket.socket()
s.bind(("127.0.0.1", 0))
s.listen(1)
sys.stdout.write("%d\n" % s.getsockname()[1])
sys.stdout.flush()
time.sleep(60)
' >"$listener_out" 2>/dev/null &
listener_pid=$!

listener_port=""
for _ in $(seq 1 100); do
  listener_port=$(head -n 1 "$listener_out")
  [[ -n "$listener_port" ]] && break
  kill -0 "$listener_pid" 2>/dev/null || break
  sleep 0.1
done

if [[ -z "$listener_port" ]]; then
  # Without a listener there is nothing to assert, and asserting anyway would
  # report the helper as broken when it is the fixture that never came up.
  echo "FAIL live-port fixture never started listening"
  fail=1
else
  if ci_wait_port 127.0.0.1 "$listener_port" 10 >/dev/null 2>&1; then
    echo "ok   wait on a live port succeeds"
  else
    echo "FAIL wait on a live port should succeed"
    fail=1
  fi
  if [[ -e "$CI_BENCH_FAILURE_LOG" ]]; then
    echo "FAIL a satisfied wait wrote a digest entry"
    sed 's/^/     | /' "$CI_BENCH_FAILURE_LOG"
    fail=1
  else
    echo "ok   a satisfied wait leaves no digest"
  fi
fi
kill "$listener_pid" 2>/dev/null || true
wait "$listener_pid" 2>/dev/null || true

# A digest that cannot be written must not turn the wait's failure into a
# failure of its own.
CI_BENCH_FAILURE_LOG=/proc/nonexistent/failures.log
if ci_wait_port 127.0.0.1 1 1 >/dev/null 2>&1; then
  echo "FAIL wait on a dead port should fail"
  fail=1
else
  echo "ok   unwritable digest still fails the wait, and nothing else"
fi

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
