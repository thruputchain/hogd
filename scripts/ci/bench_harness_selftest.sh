#!/usr/bin/env bash
# Self-test for the step-capture helpers in bench_harness.sh: exit-status
# rendering, per-step log capture, the end-of-job failure report that replays a
# failed step's log tail, and the kill-and-wait that frees the benchmark ports.
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

## ci_killall_pgrep_pattern

check_eq "pattern joins names with alternation" "loadgen_server|hog-exec" \
  "$(ci_killall_pgrep_pattern "loadgen_server hog-exec")"
# An unescaped dot matches any character, and a stray separator leaves an empty
# alternative that [pgrep -x] would match against every process name.
check_eq "pattern escapes dots and trims separators" 'chuckd\.dev|scpd\.dev' \
  "$(ci_killall_pgrep_pattern "  chuckd.dev   scpd.dev ")"

## ci_killall_wait_script

# The script runs the real [killall] through [sudo], so give it a [sudo] that
# just drops its own flags, and processes of our own to kill. The names stay
# under the 15 characters [pgrep] and [killall] compare against.
shim_dir="$work_dir/bin"
mkdir -p "$shim_dir"
cat >"$shim_dir/sudo" <<'SHIM'
#!/usr/bin/env bash
while [[ $1 == -* ]]; do shift; done
exec "$@"
SHIM
chmod +x "$shim_dir/sudo"
cp "$(command -v sleep)" "$shim_dir/ci-st-sleeper"
cp "$(command -v bash)" "$shim_dir/ci-st-ignorer"

run_kill_script() {
  local names=$1 wait_sec=$2
  PATH="$shim_dir:$PATH" bash -c "$(ci_killall_wait_script "$names" "$wait_sec")"
}

"$shim_dir/ci-st-sleeper" 300 &
sleeper_pid=$!
if run_kill_script "ci-st-sleeper" 10 2>/dev/null; then
  echo "ok   kill script reports the process gone"
else
  echo "FAIL kill script should have killed ci-st-sleeper"
  fail=1
fi
# The whole point is that the script does not return until the process is
# actually gone: that is what makes the ports it held free for the next suite.
if pgrep -x ci-st-sleeper >/dev/null 2>&1; then
  echo "FAIL kill script returned with ci-st-sleeper still running"
  fail=1
else
  echo "ok   kill script waited for the process to exit"
fi
wait "$sleeper_pid" 2>/dev/null || true

# A process that ignores SIGTERM must be escalated to SIGKILL rather than
# outliving the kill and holding its ports. The sleep runs in a loop so bash
# cannot exec it in place, which would rename the process out from under
# [pgrep].
"$shim_dir/ci-st-ignorer" -c 'trap "" TERM; while :; do sleep 1; done' &
ignorer_pid=$!
if run_kill_script "ci-st-ignorer" 2 2>/dev/null; then
  echo "ok   kill script escalates to SIGKILL"
else
  echo "FAIL kill script should have SIGKILLed ci-st-ignorer"
  fail=1
fi
check_eq "SIGKILLed process is gone" "" "$(pgrep -x ci-st-ignorer || true)"
kill -KILL "$ignorer_pid" 2>/dev/null || true
wait "$ignorer_pid" 2>/dev/null || true

# A kill that never takes has to be reported, not swallowed: the alternative is
# a bind failure in the next suite that names no host.
cat >"$shim_dir/sudo" <<'SHIM'
#!/usr/bin/env bash
exit 0
SHIM
chmod +x "$shim_dir/sudo"
"$shim_dir/ci-st-sleeper" 300 &
survivor_pid=$!
kill_report=$(run_kill_script "ci-st-sleeper" 1 2>&1 >/dev/null) && {
  echo "FAIL kill script should fail when the process survives"
  fail=1
}
check_contains "surviving process is named" "ci-st-sleeper" "$kill_report"
kill -KILL "$survivor_pid" 2>/dev/null || true
wait "$survivor_pid" 2>/dev/null || true

## ci_remote_killall_hosts

# Stand in for ssh so the helper's fan-out and its per-host status reporting can
# run here. [sudo] is still the no-op above, so every host "fails".
ci_remote_run() {
  local host=$1
  shift
  PATH="$shim_dir:$PATH" HOSTNAME_OVERRIDE="$host" bash -c "$*"
}
"$shim_dir/ci-st-sleeper" 300 &
survivor_pid=$!
CI_KILLALL_WAIT_SEC=1
hosts_report=$(ci_remote_killall_hosts "ci-st-sleeper" hogA hogB 2>&1 >/dev/null) && {
  echo "FAIL ci_remote_killall_hosts should fail when a host is not clear"
  fail=1
}
check_contains "unclear host named" "hogA" "$hosts_report"
check_contains "every unclear host named" "hogB" "$hosts_report"
kill -KILL "$survivor_pid" 2>/dev/null || true
wait "$survivor_pid" 2>/dev/null || true

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
