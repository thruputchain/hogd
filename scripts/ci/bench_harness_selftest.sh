#!/usr/bin/env bash
# Self-test for the step-capture helpers in bench_harness.sh: exit-status
# rendering, per-step log capture, the end-of-job failure report that replays a
# failed step's log tail, and the stale-process kill that has to leave a host
# actually clear before the next suite launches on it.
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

## ci_killall_script / ci_remote_killall_hosts
#
# The kill runs on a remote host, so the test gives it stub [sudo], [killall]
# and [pgrep] on PATH and a file of "running" process names for them to act on.
# The stub [sudo] rewrites the absolute [/usr/bin/killall] the real script uses
# back to the stub on PATH; everything else is the script under test.

stub_dir="$work_dir/stubs"
mkdir -p "$stub_dir"

cat >"$stub_dir/sudo" <<'STUB'
#!/usr/bin/env bash
[[ -e "$STUB_STATE/no_sudo" ]] && exit 1
[[ $1 == -n ]] && shift
cmd=$1
shift
[[ $cmd == /usr/bin/killall ]] && cmd=killall
exec "$cmd" "$@"
STUB

# Signals what it is asked to signal: a name goes away unless the case under
# test declared it deaf to that signal. Exits 1 when nothing matched, as
# killall does.
cat >"$stub_dir/killall" <<'STUB'
#!/usr/bin/env bash
signal=TERM
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
matched=1
for name in "${names[@]}"; do
  grep -qxF "$name" "$STUB_STATE/running" 2>/dev/null || continue
  matched=0
  grep -qxF "$name" "$STUB_STATE/unkillable" 2>/dev/null && continue
  [[ $signal == TERM ]] && grep -qxF "$name" "$STUB_STATE/deaf_to_term" \
    2>/dev/null && continue
  grep -vxF "$name" "$STUB_STATE/running" >"$STUB_STATE/running.tmp" || true
  mv "$STUB_STATE/running.tmp" "$STUB_STATE/running"
done
exit "$matched"
STUB

cat >"$stub_dir/pgrep" <<'STUB'
#!/usr/bin/env bash
list=0
name=""
while (($#)); do
  case $1 in
  -x) ;;
  -l) list=1 ;;
  --) ;;
  *) name=$1 ;;
  esac
  shift
done
grep -qxF "$name" "$STUB_STATE/running" 2>/dev/null || exit 1
((list)) && echo "1234 $name"
exit 0
STUB

chmod +x "$stub_dir/sudo" "$stub_dir/killall" "$stub_dir/pgrep"

# Runs the generated kill program against a stub host. $1 is the list of names
# left running on it; the rest name the state files to seed.
run_kill_stub() {
  local running=$1
  shift
  local state="$work_dir/stub_state"
  local marker

  rm -rf "$state"
  mkdir -p "$state"
  printf '%s\n' $running >"$state/running"
  [[ -n $running ]] || : >"$state/running"
  for marker in "$@"; do
    case $marker in
    no_sudo) : >"$state/no_sudo" ;;
    deaf_to_term) printf '%s\n' $running >"$state/deaf_to_term" ;;
    unkillable) printf '%s\n' $running >"$state/unkillable" ;;
    esac
  done

  STUB_STATE="$state" PATH="$stub_dir:$PATH" \
    bash -c "$(ci_killall_script 'hog-exec chuckd' 1 1)" 2>&1
}

out=$(run_kill_stub "") || {
  echo "FAIL kill on an already-clear host should succeed"
  fail=1
}
check_eq "clear host needs no escalation" "" "$out"

out=$(run_kill_stub "hog-exec chuckd") || {
  echo "FAIL kill should succeed when SIGTERM is enough"
  fail=1
}
check_eq "SIGTERM alone needs no escalation" "" "$out"

# The failure this whole path exists for: a process that outlives SIGTERM keeps
# its listening socket, and the next suite dies on "Address already in use".
out=$(run_kill_stub "hog-exec" deaf_to_term) || {
  echo "FAIL kill should succeed once SIGKILL lands"
  fail=1
}
check_contains "SIGTERM survivor is escalated to SIGKILL" \
  "escalating to SIGKILL" "$out"

out=$(run_kill_stub "hog-exec" unkillable) && {
  echo "FAIL kill should fail when a process survives SIGKILL"
  fail=1
}
check_contains "survivor is named" "hog-exec" "$out"

# A missing sudoers rule must not silently disarm the kill.
out=$(run_kill_stub "hog-exec" no_sudo) || {
  echo "FAIL kill should fall back to unprivileged killall"
  fail=1
}
check_eq "unprivileged fallback needs no escalation" "" "$out"

# The per-host reporting: an unreachable host and a host that could not be
# cleared are both failures, and both have to name the host.
ci_remote_run() {
  local host=$1
  shift
  case $host in
  unreachable) return 255 ;;
  busy) return 1 ;;
  esac
  return 0
}

if ci_remote_killall_hosts "hog-exec" clean1 clean2 >/dev/null 2>&1; then
  echo "ok   kill succeeds when every host comes back clear"
else
  echo "FAIL kill should succeed when every host comes back clear"
  fail=1
fi

out=$(ci_remote_killall_hosts "hog-exec" clean1 busy 2>&1) && {
  echo "FAIL kill should fail when a host could not be cleared"
  fail=1
}
check_contains "uncleared host is named" \
  "stale benchmark processes survived on busy" "$out"

out=$(ci_remote_killall_hosts "hog-exec" unreachable 2>&1) && {
  echo "FAIL kill should fail when a host is unreachable"
  fail=1
}
check_contains "unreachable host is named" \
  "unreachable is unreachable" "$out"

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
