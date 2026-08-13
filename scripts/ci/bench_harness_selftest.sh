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
#
# The script runs against a stubbed host: [procs] is the process table, one
# "<name> <pid> <state> <dies-on>" line per process, and the stubs for
# [pgrep]/[ps]/[killall] read and edit it the way the real ones would. Every
# case asserts what the host looks like afterwards, not just the exit status,
# because "the host is clear" is the whole contract.

stub_dir="$work_dir/bin"
mkdir -p "$stub_dir"
procs="$work_dir/procs"
signals="$work_dir/signals"

cat >"$stub_dir/pgrep" <<'STUB'
#!/usr/bin/env bash
# Usage: pgrep -x <name>. Lists every pid under that name, zombies included,
# exactly as the real pgrep does.
name=${!#}
awk -v n="$name" '$1 == n { print $2 }' "$STUB_PROCS"
STUB

cat >"$stub_dir/ps" <<'STUB'
#!/usr/bin/env bash
# Usage: ps -o stat= -p <pid>.
pid=${!#}
awk -v p="$pid" '$2 == p { print $3 }' "$STUB_PROCS"
STUB

cat >"$stub_dir/killall" <<'STUB'
#!/usr/bin/env bash
# Usage: killall -q -s <SIG> <name>...
#
# Exits 0 only when at least one process was killed for EVERY name, which is
# what killall(1) documents and what psmisc does -- it reports failure while
# killing what it did find. Nothing may read this status as "the host is clear".
signal=""
names=()
while (($#)); do
  case $1 in
  -q) ;;
  -s)
    signal=$2
    shift
    ;;
  *) names+=("$1") ;;
  esac
  shift
done
echo "$signal ${names[*]}" >>"$STUB_SIGNALS"
status=0
for name in "${names[@]}"; do
  killed=1
  while read -r p pid state dies_on; do
    [[ "$p" == "$name" ]] || continue
    # A process only leaves when the signal is one it dies on; a zombie is
    # already gone but still signallable, so it stays in the table.
    if [[ "$dies_on" == "$signal" || ("$dies_on" == TERM && "$signal" == KILL) ]]; then
      killed=0
      grep -v "^$p $pid " "$STUB_PROCS" >"$STUB_PROCS.new" || true
      mv "$STUB_PROCS.new" "$STUB_PROCS"
    fi
  done <"$STUB_PROCS"
  [[ "$killed" == 0 ]] || status=1
done
exit "$status"
STUB

cat >"$stub_dir/sudo" <<'STUB'
#!/usr/bin/env bash
# Usage: sudo -n <command>... Refuses when STUB_SUDO_DENIED is set, the way
# [sudo -n] does on a host with no passwordless rule: status 1 and a message,
# which is the only thing distinguishing it from a [killall] that matched
# nothing.
if [[ -n "${STUB_SUDO_DENIED:-}" ]]; then
  echo "sudo: a password is required" >&2
  exit 1
fi
shift # -n
exec "$@"
STUB

chmod +x "$stub_dir"/{pgrep,ps,killall,sudo}

# Runs the generated script against the stubbed host and reports its status in
# [killall_status], its stderr in [killall_stderr], and the signals it sent in
# [killall_signals].
run_killall_script() {
  local table=$1
  local wait_sec=${2:-1}
  local path=${3:-$stub_dir:$PATH}

  printf '%s\n' "$table" >"$procs"
  : >"$signals"
  killall_stderr=$(
    PATH="$path" STUB_PROCS="$procs" STUB_SIGNALS="$signals" \
      CI_KILLALL_BIN="$stub_dir/killall" \
      bash -c "$(CI_KILLALL_BIN="$stub_dir/killall" \
        ci_killall_script 'hog-exec hog-mempool' "$wait_sec")" 2>&1 >/dev/null
  ) && killall_status=0 || killall_status=$?
  killall_signals=$(tr '\n' ';' <"$signals")
  killall_survivors=$(cat "$procs")
}

# A clear host: one [killall] that matches nothing, no escalation, no wait.
run_killall_script ""
check_eq "clear host succeeds" "0" "$killall_status"
check_eq "clear host is not escalated to SIGKILL" "TERM hog-exec hog-mempool;" \
  "$killall_signals"

# The ordinary case: the processes take SIGTERM and go.
run_killall_script "hog-exec 101 S TERM"
check_eq "SIGTERM survivor-free host succeeds" "0" "$killall_status"
check_eq "SIGTERM alone needs no escalation" "TERM hog-exec hog-mempool;" \
  "$killall_signals"
check_eq "SIGTERM leaves the host clear" "" "$killall_survivors"

# The failure this change exists for: a process that outlives SIGTERM still
# holds its ports, so it has to be escalated rather than left for the next
# suite to collide with.
#
# Note what the table does not have: hog-exec. Every real host runs a subset of
# the names, so [killall] reports failure on the absent ones even as it signals
# the one that is there -- reading that status as "clear" would skip the wait and
# the escalation on precisely the host that still holds the ports.
run_killall_script "hog-mempool 202 S KILL"
check_eq "SIGTERM survivor host succeeds" "0" "$killall_status"
check_contains "escalation sends SIGKILL" "KILL hog-exec hog-mempool" \
  "$killall_signals"
check_eq "escalation leaves the host clear" "" "$killall_survivors"

# Nothing left to try: name the survivor so the warning points at the host that
# will fail the next suite's bind.
run_killall_script "hog-exec 303 S never"
check_eq "unkillable process fails" "1" "$killall_status"
check_contains "unkillable process is named" "hog-exec(303)" "$killall_stderr"

# A zombie holds no ports and will never leave the table on its own, so it must
# not be mistaken for a live process and spend both deadlines.
run_killall_script "hog-exec 404 Z never"
check_eq "zombie does not fail the host" "0" "$killall_status"
check_eq "zombie is not escalated to SIGKILL" "TERM hog-exec hog-mempool;" \
  "$killall_signals"

# With no passwordless sudo the unprivileged [killall] still has to run: the
# benchmark processes belong to the CI user either way. A [sudo -n] that could
# not run at all used to be indistinguishable from a host with nothing on it.
export STUB_SUDO_DENIED=1
run_killall_script "hog-exec 505 S TERM"
unset STUB_SUDO_DENIED
check_eq "unprivileged fallback succeeds" "0" "$killall_status"
check_eq "unprivileged fallback clears the host" "" "$killall_survivors"

# A host that cannot be observed can still be insisted upon: without [pgrep]
# there is nothing to wait on, so the escalation happens unconditionally rather
# than not at all.
nopgrep_dir="$work_dir/nopgrep"
mkdir -p "$nopgrep_dir"
for tool in bash awk grep mv cat tr date sleep; do
  ln -sf "$(command -v "$tool")" "$nopgrep_dir/$tool"
done
ln -sf "$stub_dir/ps" "$nopgrep_dir/ps"
ln -sf "$stub_dir/sudo" "$nopgrep_dir/sudo"
run_killall_script "hog-exec 606 S KILL" 1 "$nopgrep_dir"
check_eq "host without pgrep succeeds" "0" "$killall_status"
check_contains "host without pgrep is still escalated" "KILL hog-exec" \
  "$killall_signals"
check_eq "host without pgrep is cleared" "" "$killall_survivors"

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
