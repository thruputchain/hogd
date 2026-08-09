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

## ci_control_stop_hosts / ci_control_shutdown_hosts
#
# A server that could not be reached is the ordinary case -- the schedule shuts
# each host down twice -- while a server that answers and reports it could not
# clear the host must not be passed over in silence.

control_cli_stub="$work_dir/control_cli"
cat >"$control_cli_stub" <<'STUB'
#!/usr/bin/env bash
# Exits with whatever $1 (the host name) says to, so each case can pick a
# status per host: "ok" 0, "gone" 1 (unreachable), "dirty" 2 (server error).
case "$1" in
ok) exit 0 ;;
gone) exit 1 ;;
dirty) exit 2 ;;
notfound) exit 127 ;;
signalled) exit 137 ;;
esac
exit 0
STUB
chmod +x "$control_cli_stub"

status=0
out=$(ci_control_stop_hosts "$control_cli_stub" 11111 ok ok 2>&1) || status=$?
check_eq "every host clear succeeds" "0" "$status"
check_eq "a clear run says nothing" "" "$out"

status=0
out=$(ci_control_shutdown_hosts "$control_cli_stub" 11111 ok gone 2>&1) || status=$?
check_eq "an unreachable server is tolerated" "0" "$status"
check_eq "an unreachable server is not reported" "" "$out"

status=0
out=$(ci_control_stop_hosts "$control_cli_stub" 11111 ok dirty 2>&1) || status=$?
check_eq "a host that could not be cleared fails" "1" "$status"
check_contains "the uncleared host is named" "dirty" "$out"

status=0
out=$(ci_control_shutdown_hosts "$control_cli_stub" 11111 dirty gone dirty 2>&1) ||
  status=$?
check_eq "shutdown reports an uncleared host" "1" "$status"
check_eq "every uncleared host is named" "2" \
  "$(grep -c 'failed shutdown' <<<"$out")"

# A status neither end meant to produce is a request that never happened, not a
# host that came back clear. Tolerating only the documented unreachable status
# is what keeps these from passing.
status=0
out=$(ci_control_stop_hosts "$control_cli_stub" 11111 ok notfound 2>&1) || status=$?
check_eq "a control_cli that could not run fails" "1" "$status"
check_contains "the unexpected status is named" "exit status 127" "$out"

status=0
out=$(ci_control_stop_hosts "$control_cli_stub" 11111 signalled 2>&1) || status=$?
check_eq "a signalled control_cli fails" "1" "$status"
check_contains "the signal is named" "SIGKILL" "$out"

# The whole helper missing must not read as every host being clear.
status=0
out=$(ci_control_stop_hosts "$work_dir/no_such_control_cli" 11111 ok 2>&1) || status=$?
check_eq "a missing control_cli fails" "1" "$status"

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
