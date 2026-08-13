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

## ci_control_log_paths

# The paths a launcher records come out of a [control_cli start] that also logs
# its own progress, so the two lines have to be picked out of that noise.
start_output=$(
  cat <<'EOF'
I[0813 00:43:44 control_cli.cc:93] Starting server: chuckd.dev bench
I[0813 00:43:44 control_cli.cc:98] Start command sent successfully
CONTROL_STDOUT_PATH=/tmp/hogd_stdout_603992681528260_2983852_BSgpmb
CONTROL_STDERR_PATH=/tmp/hogd_stderr_603992681528260_2983852_R4SDV2
EOF
)
unset out_stdout out_stderr
ci_control_log_paths "$start_output" out_stdout out_stderr
check_eq "start output stdout path" \
  "/tmp/hogd_stdout_603992681528260_2983852_BSgpmb" "$out_stdout"
check_eq "start output stderr path" \
  "/tmp/hogd_stderr_603992681528260_2983852_R4SDV2" "$out_stderr"

# A start that reported neither path leaves the caller nothing to collect, and
# must not leave it a half-assigned pair either.
unset out_stdout out_stderr
if ci_control_log_paths "no paths here" out_stdout out_stderr; then
  echo "FAIL missing paths should fail"
  fail=1
else
  echo "ok   missing paths fails"
fi
check_eq "missing paths assigns nothing" "" "${out_stdout:-}${out_stderr:-}"

# Only stderr missing is still a failure: the replay needs both streams.
unset out_stdout out_stderr
if ci_control_log_paths "CONTROL_STDOUT_PATH=/tmp/out" out_stdout out_stderr; then
  echo "FAIL a lone stdout path should fail"
  fail=1
else
  echo "ok   a lone stdout path fails"
fi

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

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
