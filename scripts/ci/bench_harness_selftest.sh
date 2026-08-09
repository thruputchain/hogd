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

## ci_killall_script

# The script runs on the benchmark hosts, so exercise it against stubs for the
# three commands it uses there: a [pgrep] and a [killall] backed by a file of
# running process names, and a [sudo] that just drops its [-n] and runs the
# rest. [$stub_dir/stubborn] names the processes that ignore SIGTERM, which is
# the case that leaves a port bound under the old fire-and-forget kill.
stub_dir="$work_dir/killall_stubs"
mkdir -p "$stub_dir/bin"
cat >"$stub_dir/bin/pgrep" <<'STUB'
#!/usr/bin/env bash
# Usage: pgrep -x NAME. Prints the pid, as the real one does, so the caller can
# read the process state out of the stub procfs.
grep -qxF "$2" "$STUB_DIR/running" || exit 1
cat "$STUB_DIR/pids/$2"
STUB
cat >"$stub_dir/bin/sudo" <<'STUB'
#!/usr/bin/env bash
[[ $1 == -n ]] && shift
exec "$@"
STUB
cat >"$stub_dir/bin/killall" <<'STUB'
#!/usr/bin/env bash
# Usage: killall -q [-KILL] NAME...
signal=TERM
while [[ $# -gt 0 ]]; do
  case $1 in
  -q) ;;
  -KILL) signal=KILL ;;
  *) break ;;
  esac
  shift
done
echo "$signal $*" >>"$STUB_DIR/signals"
status=0
for name in "$@"; do
  if ! grep -qxF "$name" "$STUB_DIR/running"; then
    # What killall reports for a name that matched nothing, which is the normal
    # case for a host running only some of these processes.
    status=1
    continue
  fi
  if [[ $signal == TERM ]] && grep -qxF "$name" "$STUB_DIR/stubborn"; then
    continue
  fi
  if [[ $signal == KILL ]] && grep -qxF "$name" "$STUB_DIR/unkillable"; then
    continue
  fi
  # A zombie is already dead; no signal moves it, and only its parent can
  # clear it.
  if grep -qxF "$name" "$STUB_DIR/zombies"; then
    continue
  fi
  grep -vxF "$name" "$STUB_DIR/running" >"$STUB_DIR/running.tmp" || true
  mv "$STUB_DIR/running.tmp" "$STUB_DIR/running"
done
exit "$status"
STUB
chmod +x "$stub_dir/bin/pgrep" "$stub_dir/bin/sudo" "$stub_dir/bin/killall"

# Runs the generated script against the stubs. Arguments are the process names
# that are running, and [$stubborn]/[$unkillable] name the ones that survive
# SIGTERM and SIGKILL respectively.
run_killall_script() {
  local name pid=1000 state

  printf '%s\n' "$@" >"$stub_dir/running"
  printf '%s\n' ${stubborn:+$stubborn} >"$stub_dir/stubborn"
  printf '%s\n' ${unkillable:+$unkillable} >"$stub_dir/unkillable"
  printf '%s\n' ${zombies:+$zombies} >"$stub_dir/zombies"
  : >"$stub_dir/signals"

  # Give each running process a pid and a procfs entry in the state it is in.
  # The pid is fixed per name for the whole run, so a kill of one name cannot
  # renumber another out from under the probe.
  rm -rf "$stub_dir/pids" "$stub_dir/proc"
  mkdir -p "$stub_dir/pids" "$stub_dir/proc"
  for name in "$@"; do
    pid=$((pid + 1))
    echo "$pid" >"$stub_dir/pids/$name"
    state=S
    [[ " ${zombies:-} " == *" $name "* ]] && state=Z
    mkdir -p "$stub_dir/proc/$pid"
    # Field 3 of a real stat line, behind the parenthesized command name.
    echo "$pid ($name) $state 1 $pid $pid 0 -1 4194304" \
      >"$stub_dir/proc/$pid/stat"
  done

  STUB_DIR="$stub_dir" PATH="$stub_dir/bin:$PATH" \
    bash -c "$(CI_KILLALL_WAIT_SEC=1 CI_KILLALL_BIN="$stub_dir/bin/killall" \
      CI_KILLALL_PROCFS="$stub_dir/proc" \
      ci_killall_script 'hog-exec hog-db')" 2>&1
}

stubborn='' unkillable='' zombies=''
killall_out=$(run_killall_script hog-exec hog-db) && killall_status=0 ||
  killall_status=$?
check_eq "reap of processes that exit on SIGTERM succeeds" "0" \
  "$killall_status"
check_eq "processes that exit on SIGTERM are not escalated to SIGKILL" \
  "TERM hog-exec hog-db" "$(cat "$stub_dir/signals")"
check_eq "successful reap is quiet" "" "$killall_out"

# The failure this fix is for: a process that outlives SIGTERM keeps its ports
# bound, so it has to be escalated rather than assumed gone.
stubborn='hog-exec' unkillable='' zombies=''
killall_out=$(run_killall_script hog-exec hog-db) && killall_status=0 ||
  killall_status=$?
check_eq "reap escalates past a stubborn process" "0" "$killall_status"
check_contains "escalation names the stubborn process" \
  "still running 1s after SIGTERM: hog-exec" "$killall_out"
check_contains "escalation sends SIGKILL to the survivor only" \
  "KILL hog-exec" "$(cat "$stub_dir/signals")"

# A process that cannot be reaped at all must fail here, where it can still be
# named, rather than as an unexplained bind failure in the next suite.
stubborn='hog-exec' unkillable='hog-exec' zombies=''
killall_out=$(run_killall_script hog-exec hog-db) && killall_status=0 ||
  killall_status=$?
check_eq "reap fails when a process survives SIGKILL" "1" "$killall_status"
check_contains "unreapable process is named" \
  "stale processes survived SIGKILL: hog-exec" "$killall_out"

# A zombie is pgrep-visible and no signal can clear it, but it exited already
# and gave up its ports. Counting one as alive would wait out both timeouts and
# then fail the reap over a process that holds nothing.
stubborn='' unkillable='' zombies='hog-exec'
killall_out=$(run_killall_script hog-exec hog-db) && killall_status=0 ||
  killall_status=$?
check_eq "a zombie does not fail the reap" "0" "$killall_status"
check_eq "a zombie is not escalated to SIGKILL" "TERM hog-exec hog-db" \
  "$(cat "$stub_dir/signals")"
check_eq "a zombie is reaped quietly" "" "$killall_out"

# A host that runs none of them is the common case, and killall reporting "no
# process found" for every name must not read as a failed reap.
stubborn='' unkillable='' zombies=''
killall_out=$(run_killall_script) && killall_status=0 || killall_status=$?
check_eq "reap of a host with nothing running succeeds" "0" "$killall_status"

## ci_remote_killall_hosts

# The per-host status has to survive back to the caller: a suite that launches
# anyway fails later on a port it cannot bind, with nothing left to say why.
ci_remote_run() {
  local host=$1
  [[ $host == unreachable ]] && return 255
  return 0
}
if ci_remote_killall_hosts "hog-exec" hog5 hog9 2>/dev/null; then
  echo "ok   reaping every host reports success"
else
  echo "FAIL reaping every host should report success"
  fail=1
fi
killall_out=$(ci_remote_killall_hosts "hog-exec" hog5 unreachable 2>&1) &&
  {
    echo "FAIL a host that could not be reaped should report failure"
    fail=1
  }
check_contains "unreaped host is named" \
  "failed to reap stale benchmark processes on unreachable" "$killall_out"

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
