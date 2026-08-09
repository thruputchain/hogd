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

## ci_control_start / ci_expect_running

fake_cli="$work_dir/fake_control_cli"
cat >"$fake_cli" <<'CLI'
#!/usr/bin/env bash
# Stands in for control_cli: <host> <port> <command> <args...>
case $3 in
start)
  echo "Starting server: ${*:4}"
  echo "CONTROL_STDOUT_PATH=$FAKE_STDOUT"
  echo "CONTROL_STDERR_PATH=$FAKE_STDERR"
  ;;
expect_running)
  echo "Process is not running: exit code 127"
  exit 1
  ;;
esac
CLI
chmod +x "$fake_cli"

export FAKE_STDOUT="$work_dir/proc.out" FAKE_STDERR="$work_dir/proc.err"
echo 'coordinator starting' >"$FAKE_STDOUT"
echo 'chuckd.dev: error while loading shared libraries: libc.so.6' \
  >"$FAKE_STDERR"

# The replay reads the logs over ssh in production; here the "remote" host is
# this one, so run the command locally.
ci_remote_run() {
  shift
  bash -c "$*"
}

CONTROL_LOG_RECORDS=()
ci_control_start "$fake_cli" hog4 11111 chuckd chuckd.dev --seed 1 >/dev/null
check_eq "start records the paths the control server captured" \
  "hog4|chuckd|$FAKE_STDOUT|$FAKE_STDERR" "${CONTROL_LOG_RECORDS[0]}"

report=$(ci_expect_running "$fake_cli" 11111 hog4 2>&1) && {
  echo "FAIL expect_running should fail when the process is gone"
  fail=1
}
check_contains "expect_running names the dead process and its host" \
  "last 80 lines of chuckd on hog4" "$report"
# The whole point: an exit 127 that used to arrive with no explanation now
# arrives with the loader error that produced it.
check_contains "expect_running replays the dead process's stderr" \
  "error while loading shared libraries" "$report"
check_contains "expect_running replays the dead process's stdout" \
  "coordinator starting" "$report"

# The control server tracks one process per host, and expect_running reports on
# the last one started, so that is the one whose log must be replayed.
CONTROL_LOG_RECORDS=()
ci_control_start "$fake_cli" hog9 11111 event hogd/hog_event/hog-event >/dev/null
ci_control_start "$fake_cli" hog9 11111 indexer hogd/hog_indexer/hog-indexer \
  >/dev/null
report=$(ci_expect_running "$fake_cli" 11111 hog9 2>&1) || true
check_contains "expect_running replays the most recent launch on the host" \
  "of indexer on hog9" "$report"

# A host with nothing recorded must still say so rather than failing silently.
CONTROL_LOG_RECORDS=()
report=$(ci_expect_running "$fake_cli" 11111 hog5 2>&1) || true
check_contains "expect_running reports a host with no captured output" \
  "no captured output for the process on hog5" "$report"

# A control_cli that did not print the paths is a launch that cannot be
# diagnosed later, so fail at the launch rather than at the replay.
broken_cli="$work_dir/broken_control_cli"
printf '#!/usr/bin/env bash\necho "Start command sent successfully"\n' \
  >"$broken_cli"
chmod +x "$broken_cli"
CONTROL_LOG_RECORDS=()
report=$(ci_control_start "$broken_cli" hog4 11111 chuckd chuckd.dev 2>&1) && {
  echo "FAIL start should fail when the log paths are missing"
  fail=1
}
check_contains "start rejects a launch with no captured paths" \
  "failed to parse control cli log paths for chuckd on hog4" "$report"

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
