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

# [killall -q -SIG names...] records the signal it was asked for and clears
# $ALIVE_FILE only for the signals in $DIES_ON, standing in for a process that
# ignores SIGTERM. $KILLALL_FAILS makes it fail the way a host with a broken
# killall does: nonzero, with a complaint on stderr.
# A signal that matches nothing exits 1 silently, which is what [killall -q]
# does and must not be read as a failure.
cat >"$reap_bin/killall" <<'FAKE_KILLALL'
#!/usr/bin/env bash
signal=""
for arg in "$@"; do
  case $arg in
  -TERM | -KILL) signal=${arg#-} ;;
  esac
done
echo "$signal" >>"$SIGNAL_FILE"
if [[ -n ${KILLALL_FAILS:-} ]]; then
  echo "killall: operation not permitted" >&2
  exit 1
fi
[[ " $DIES_ON " != *" $signal "* ]] || : >"$ALIVE_FILE"
[[ -s "$ALIVE_FILE" ]] || exit 1
FAKE_KILLALL
chmod +x "$reap_bin/ps" "$reap_bin/killall"

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
      KILLALL_FAILS="${KILLALL_FAILS:-}" \
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

# A host whose killall is broken has not been shown to have a process
# refusing to die, so it must not report one: that reading fails the benchmark
# step, and sends whoever reads it after a process nothing ever signalled.
KILLALL_FAILS=1 run_reap "hog-mempool" "TERM KILL"
check_eq "unsendable signal is not reported as a survivor" "3" "$reap_status"
check_contains "unsendable signal says what went wrong" \
  "cannot signal processes: killall: operation not permitted" "$reap_output"
check_eq "unsendable signal does not wait out the timeouts" "TERM" \
  "$(cat "$signal_file")"
if [[ "$reap_output" == *"alive after SIGKILL"* ]]; then
  echo "FAIL unsendable signal must not claim a SIGKILL survivor"
  fail=1
else
  echo "ok   unsendable signal makes no SIGKILL claim"
fi

## ci_wait_control_server

# The gate exists because a stale control server holds the port open and
# answers a connect probe exactly as this run's server would, so waiting on
# the port let a benchmark proceed against the wrong build (hogd issue #3940).
# What it has to wait on is the RPC verdict, not the port.
control_bin="$work_dir/control_bin"
mkdir -p "$control_bin"
attempt_file="$work_dir/ready_attempts"

# Stands in for [control_cli <host> <port> ready]: fails for the first
# $READY_AFTER calls, then succeeds. NOT_READY_HOSTS never succeeds, standing
# in for a host still serving a previous run's build.
cat >"$control_bin/control_cli" <<'FAKE_CONTROL_CLI'
#!/usr/bin/env bash
host=$1
if [[ " ${NOT_READY_HOSTS:-} " == *" $host "* ]]; then
  echo "Control server on $host:$2 is not ready: kCIServerErrorCodeVersionMismatch"
  exit 1
fi
seen=$(cat "$ATTEMPT_FILE.$host" 2>/dev/null || echo 0)
echo $((seen + 1)) >"$ATTEMPT_FILE.$host"
if ((seen < ${READY_AFTER:-0})); then
  echo "Error: connection refused"
  exit 1
fi
exit 0
FAKE_CONTROL_CLI
chmod +x "$control_bin/control_cli"

wait_status=0
wait_output=""
# Waits on $@ (hosts) where every host but $NOT_READY_HOSTS becomes ready
# after $READY_AFTER failed probes.
run_wait_control() {
  rm -f "$attempt_file".*
  wait_status=0
  wait_output=$(
    CI_WAIT_CONTROL_TIMEOUT_SEC=1 CI_WAIT_CONTROL_POLL_SEC=0.05 \
      ATTEMPT_FILE="$attempt_file" READY_AFTER="${READY_AFTER:-0}" \
      NOT_READY_HOSTS="${NOT_READY_HOSTS:-}" \
      ci_wait_control_server_hosts "$control_bin/control_cli" 11111 "$@" 2>&1
  ) || wait_status=$?
}

run_wait_control hog4
check_eq "a serving host passes the gate" "0" "$wait_status"
check_contains "a serving host is named" \
  "Control server on hog4:11111 is serving this build" "$wait_output"

# A host that is merely slow to come up must be waited out, not failed: the
# bench servers build concurrently with the job that drives them.
READY_AFTER=3 run_wait_control hog4
check_eq "a slow host is waited out" "0" "$wait_status"
check_eq "a slow host is polled until ready" "4" "$(cat "$attempt_file.hog4")"

# The failure this gate replaces was a version mismatch discovered by the
# first real RPC, which aborted the benchmark. Time out here instead, and
# carry the verdict so the log says stale server rather than absent one.
NOT_READY_HOSTS="hog7" run_wait_control hog7
check_eq "a stale host fails the gate" "1" "$wait_status"
check_contains "a timeout names the host" \
  "waiting for this build's control server on hog7:11111" "$wait_output"
check_contains "a timeout carries the RPC verdict" \
  "kCIServerErrorCodeVersionMismatch" "$wait_output"

# Every host is waited on even after one fails, so one stale server does not
# hide the rest and send whoever reads the log back for another run.
NOT_READY_HOSTS="hog7 hog8" run_wait_control hog4 hog7 hog8
check_eq "one stale host fails the whole gate" "1" "$wait_status"
check_contains "the first stale host is named" "on hog7:11111" "$wait_output"
check_contains "the second stale host is named" "on hog8:11111" "$wait_output"
check_contains "a ready host is still reported" \
  "Control server on hog4:11111 is serving this build" "$wait_output"

## reap_control_server.sh

# The reap has to establish that the port is free, not that no process by a
# given name is alive: [killall control_server] reporting nothing to kill
# while the port stayed bound is what turned the next bind into a core dump.
reap_port_bin="$work_dir/reap_port_bin"
mkdir -p "$reap_port_bin"

# Stands in for [ss -Hltnp "sport = :<port>"]. $SS_LISTENERS is the output,
# empty when the port is free; $SS_STATE_FILE, when set, is read instead so a
# fake holder can free the port mid-run; $SS_FAILS makes the query itself
# fail, the way an unsupported filter or a netlink error would.
cat >"$reap_port_bin/ss" <<'FAKE_SS'
#!/usr/bin/env bash
if [[ -n ${SS_FAILS:-} ]]; then
  echo "ss: bad filter" >&2
  exit 1
fi
if [[ -n ${SS_STATE_FILE:-} ]]; then
  cat "$SS_STATE_FILE" 2>/dev/null
  exit 0
fi
printf '%s' "${SS_LISTENERS:-}"
FAKE_SS
cat >"$reap_port_bin/killall" <<'FAKE_KILLALL_PORT'
#!/usr/bin/env bash
echo "killall $*" >>"$KILLALL_LOG"
FAKE_KILLALL_PORT

# Stands in for [control_cli <host> <port> shutdown]. $RPC_SHUTDOWN_WORKS
# frees the port the way a server honoring the RPC would; otherwise it
# refuses the way a server predating version-blind shutdown does.
cat >"$reap_port_bin/control_cli" <<'FAKE_REAP_CLI'
#!/usr/bin/env bash
echo "control_cli $*" >>"$CLI_LOG"
if [[ -n ${RPC_SHUTDOWN_WORKS:-} ]]; then
  : >"${SS_STATE_FILE:?}"
  exit 0
fi
echo "Failed to send shutdown: kCIServerErrorCodeVersionMismatch" >&2
exit 1
FAKE_REAP_CLI
chmod +x "$reap_port_bin/ss" "$reap_port_bin/killall" \
  "$reap_port_bin/control_cli"

killall_log="$work_dir/reap_killall_log"
cli_log="$work_dir/reap_cli_log"
ss_state="$work_dir/reap_ss_state"
reap_port_status=0
reap_port_output=""
# Runs the reap against a port whose listeners are $1.
run_reap_port() {
  : >"$killall_log"
  : >"$cli_log"
  reap_port_status=0
  reap_port_output=$(
    PATH="$reap_port_bin:$PATH" SS_LISTENERS="$1" KILLALL_LOG="$killall_log" \
      SS_FAILS="${SS_FAILS:-}" SS_STATE_FILE="${SS_STATE_FILE:-}" \
      CLI_LOG="$cli_log" RPC_SHUTDOWN_WORKS="${RPC_SHUTDOWN_WORKS:-}" \
      REAP_CONTROL_CLI="${REAP_CONTROL_CLI:-$reap_port_bin/control_cli}" \
      REAP_TERM_TIMEOUT_SEC=0 REAP_KILL_TIMEOUT_SEC=0 \
      REAP_RPC_TIMEOUT_SEC=0 \
      "$CI_BENCH_HARNESS_DIR/reap_control_server.sh" 11111 2>&1
  ) || reap_port_status=$?
}

run_reap_port ""
check_eq "a free port reaps cleanly" "0" "$reap_port_status"
check_eq "a free port is not signalled" "" "$(cat "$killall_log")"

# A listener [ss] cannot attribute to a pid is another user's process, which
# this host may not signal. Report the holder here, where it is still visible,
# rather than letting the bind abort with only an errno.
run_reap_port 'LISTEN 0 128 *:11111 *:*'
check_eq "an unreapable holder fails the reap" "1" "$reap_port_status"
check_contains "an unreapable holder is named" \
  "could not free port 11111" "$reap_port_output"
check_contains "an unreapable holder is shown" "*:11111" "$reap_port_output"
check_contains "an unreapable holder is still signalled by name" \
  "killall -TERM control_server" "$(cat "$killall_log")"

# A query that could not run has not shown the port to be free. Reporting no
# output as an empty port would let the bind this reap protects abort anyway,
# which is the whole failure the script exists to prevent.
SS_FAILS=1 run_reap_port ""
check_eq "a failed query fails the reap" "1" "$reap_port_status"
check_contains "a failed query says it could not establish the port is free" \
  "cannot establish that the port is free" "$reap_port_output"
if [[ "$reap_port_output" == *"port 11111 is free"* ]]; then
  echo "FAIL a failed query must not report the port as free"
  fail=1
else
  echo "ok   a failed query makes no free-port claim"
fi

# A host without [ss] cannot be reaped by port. Fall back to the by-name reap
# this script replaced instead of failing: the weaker reap still serves the
# host, where failing here would break one the old reap handled.
: >"$killall_log"
no_ss_status=0
no_ss_output=$(
  PATH="$reap_port_bin:$PATH" KILLALL_LOG="$killall_log" \
    REAP_SS=ss_not_installed \
    "$CI_BENCH_HARNESS_DIR/reap_control_server.sh" 11111 2>&1
) || no_ss_status=$?
check_eq "a host without ss still reaps" "0" "$no_ss_status"
check_contains "a host without ss says so" "reaping by name only" \
  "$no_ss_output"
check_contains "a host without ss reaps by name" \
  "killall -TERM control_server" "$(cat "$killall_log")"

# A control_server honors the shutdown RPC no matter which commit built it or
# which user owns it, where a signal needs the holder to be this user's
# process. So the reap asks over RPC first and signals only what refuses: a
# stale server of any vintage exits itself, children and all, and a holder
# this user may not signal (hogd issue #3945) comes down without root.
printf 'LISTEN 0 128 *:11111 *:*\n' >"$ss_state"
SS_STATE_FILE="$ss_state" RPC_SHUTDOWN_WORKS=1 run_reap_port ""
check_eq "a holder honoring the shutdown RPC reaps cleanly" "0" \
  "$reap_port_status"
check_contains "the shutdown RPC is asked first" \
  "control_cli localhost 11111 shutdown" "$(cat "$cli_log")"
check_eq "a holder honoring the shutdown RPC is not signalled" "" \
  "$(cat "$killall_log")"

# A holder that refuses the RPC -- a server predating version-blind shutdown,
# or something else entirely on the port -- still gets the signal reap.
run_reap_port 'LISTEN 0 128 *:11111 *:*'
check_contains "a refusing holder is still asked over RPC" \
  "control_cli localhost 11111 shutdown" "$(cat "$cli_log")"
check_contains "a refusing holder is still signalled by name" \
  "killall -TERM control_server" "$(cat "$killall_log")"

# The reap can run where control_cli was never built. That host loses the
# RPC ask, not the reap: say so and fall through to the signals.
REAP_CONTROL_CLI="$work_dir/control_cli_not_built" \
  run_reap_port 'LISTEN 0 128 *:11111 *:*'
check_eq "a missing control_cli does not break the reap" "1" \
  "$reap_port_status"
check_contains "a missing control_cli is reported" "reaping by signal only" \
  "$reap_port_output"
check_contains "a missing control_cli still signals by name" \
  "killall -TERM control_server" "$(cat "$killall_log")"

if ((fail)); then
  echo "bench_harness selftest FAILED"
  exit 1
fi
echo "bench_harness selftest passed"
