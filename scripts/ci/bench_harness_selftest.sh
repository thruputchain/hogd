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

## ci_reap_program

# The reap has to be synchronous: its callers bind fixed ports seconds later,
# so a process that has merely been signalled is still a process holding 8075.
# Drive the emitted program against a fake [ps]/[killall] pair so the
# escalation, the waiting, and the name matching are checked without any real
# processes.
reap_bin="$work_dir/bin"
alive_file="$work_dir/alive"
signal_file="$work_dir/signals"
mkdir -p "$reap_bin"

# [ps -eo comm=] lists the processes named in $ALIVE_FILE, one per line, which
# is the shape the real thing prints.
cat >"$reap_bin/ps" <<'FAKE_PS'
#!/usr/bin/env bash
cat "$ALIVE_FILE" 2>/dev/null
FAKE_PS

# [sudo -n /usr/bin/killall -q -SIG names...] records the signal it was asked
# for and clears $ALIVE_FILE only for the signals in $DIES_ON, standing in for
# a process that ignores SIGTERM. $SUDO_FAILS makes it fail the way a host with
# no killall or no passwordless sudo does: nonzero, with a complaint on stderr.
# A signal that matches nothing exits 1 silently, which is what [killall -q]
# does and must not be read as a failure.
cat >"$reap_bin/sudo" <<'FAKE_SUDO'
#!/usr/bin/env bash
signal=""
for arg in "$@"; do
  case $arg in
  -TERM | -KILL) signal=${arg#-} ;;
  esac
done
echo "$signal" >>"$SIGNAL_FILE"
if [[ -n ${SUDO_FAILS:-} ]]; then
  echo "sudo: a password is required" >&2
  exit 1
fi
[[ " $DIES_ON " != *" $signal "* ]] || : >"$ALIVE_FILE"
[[ -s "$ALIVE_FILE" ]] || exit 1
FAKE_SUDO
chmod +x "$reap_bin/ps" "$reap_bin/sudo"

reap_status=0
reap_output=""
# Runs the reap program against $1 (newline-free list of live process names)
# where the live processes die on the signals in $2, reaping the names in $3.
run_reap() {
  local alive=$1 dies_on=$2

  : >"$signal_file"
  : >"$alive_file"
  [ -z "$alive" ] || printf '%s\n' $alive >"$alive_file"

  reap_status=0
  reap_output=$(
    PATH="$reap_bin:$PATH" ALIVE_FILE="$alive_file" \
      SIGNAL_FILE="$signal_file" DIES_ON="$dies_on" \
      SUDO_FAILS="${SUDO_FAILS:-}" \
      bash -c "$(ci_reap_program "${3:-hog-mempool hog-exec}" 1 1)" 2>&1
  ) || reap_status=$?
}

# A host that reports no processes must still be signalled: where /proc hides
# them, skipping the kill would leave the stale process the reap exists to clear.
run_reap "" "TERM KILL"
check_eq "quiet host reaps cleanly" "0" "$reap_status"
check_eq "quiet host is still signalled" "TERM" "$(cat "$signal_file")"

run_reap "hog-mempool" "TERM KILL"
check_eq "process dying on SIGTERM reaps cleanly" "0" "$reap_status"
check_eq "process dying on SIGTERM is not SIGKILLed" "TERM" \
  "$(cat "$signal_file")"

run_reap "hog-mempool" "KILL"
check_eq "process ignoring SIGTERM reaps cleanly" "0" "$reap_status"
check_eq "process ignoring SIGTERM is escalated" "TERM KILL" \
  "$(tr '\n' ' ' <"$signal_file" | sed 's/ $//')"
check_contains "escalation names the survivor" \
  "alive after SIGTERM, escalating to SIGKILL: hog-mempool" "$reap_output"

# Surviving SIGKILL is a wedged host rather than a race, so it is reported as a
# failure instead of being handed to the next bind to discover.
run_reap "hog-mempool hog-exec" ""
check_eq "process surviving SIGKILL fails the reap" "2" "$reap_status"
check_contains "survivors of SIGKILL are named" \
  "alive after SIGKILL: hog-mempool hog-exec" "$reap_output"

# The names must be matched literally, the way [killall] targets them. Reading
# the configured [chuckd.dev] as a pattern would find this unrelated process,
# and the reap would then wait out both timeouts and fail over a process
# nothing had signalled.
run_reap "chuckdXdev" "" "chuckd.dev scpd.dev"
check_eq "a name is not matched as a pattern" "0" "$reap_status"
check_eq "an unsignalled process is not waited on" "TERM" "$(cat "$signal_file")"

# A host whose killall or sudo is broken has not been shown to have a process
# refusing to die, so it must not report one: that reading fails the benchmark
# step, and sends whoever reads it after a process nothing ever signalled.
SUDO_FAILS=1 run_reap "hog-mempool" "TERM KILL"
check_eq "unsendable signal is not reported as a survivor" "3" "$reap_status"
check_contains "unsendable signal says what went wrong" \
  "cannot signal processes: sudo: a password is required" "$reap_output"
check_eq "unsendable signal does not wait out the timeouts" "TERM" \
  "$(cat "$signal_file")"
if [[ "$reap_output" == *"alive after SIGKILL"* ]]; then
  echo "FAIL unsendable signal must not claim a SIGKILL survivor"
  fail=1
else
  echo "ok   unsendable signal makes no SIGKILL claim"
fi

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
