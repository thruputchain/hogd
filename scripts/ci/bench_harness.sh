#!/usr/bin/env bash

# Shared helpers for CI benchmark scripts. This file is intended to be sourced.

CI_BENCH_HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CI_BENCH_REPO_ROOT=$(cd "$CI_BENCH_HARNESS_DIR/../.." && pwd)

CI_WAIT_PORT_TIMEOUT_SEC=${CI_WAIT_PORT_TIMEOUT_SEC:-60}
CI_SSH_OPTS=(-n -T -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no)

# Where [ci_run_bench_step] tees each step's output, and how much of a failed
# step's log [ci_report_failed_bench_steps] replays at the end of the job.
CI_BENCH_LOG_DIR=${CI_BENCH_LOG_DIR:-bench_logs}
CI_BENCH_FAIL_TAIL_LINES=${CI_BENCH_FAIL_TAIL_LINES:-400}

# "<step name>:<exit status>" for each step that [ci_run_bench_step] saw fail.
declare -a CI_FAILED_BENCH_STEPS=()

# Renders an exit status the way a human reads it. A bare "exit status 134"
# hides the thing that matters most about it: 134 is 128+6, so the process was
# killed by SIGABRT rather than having reported a failure of its own.
ci_describe_exit_status() {
  local status=$1
  local signal name

  if [ "$status" -gt 128 ] && [ "$status" -lt 192 ]; then
    signal=$((status - 128))
    name=$(kill -l "$signal" 2>/dev/null) || name=""
    if [ -n "$name" ]; then
      printf 'exit status %s (killed by signal %s: SIG%s)' "$status" "$signal" \
        "$name"
    else
      printf 'exit status %s (killed by signal %s)' "$status" "$signal"
    fi
  elif [ "$status" -eq 124 ]; then
    printf 'exit status 124 (timed out)'
  else
    printf 'exit status %s' "$status"
  fi
}

# Runs one benchmark step, streaming its output as before and also capturing it
# to [$CI_BENCH_LOG_DIR/<name>.log]. Records the failure instead of propagating
# it, so a caller that runs several steps back to back keeps going and can
# report all of them at the end via [ci_report_failed_bench_steps].
#
# The capture exists because a schedule that runs several suites back to back
# produces a job log long enough that GitHub only serves a bounded tail of it.
# A suite that dies early has its diagnostic output evicted by the thousands of
# lines the later suites emit, so by the time the run finishes the only thing
# left is "one or more benchmark steps failed" with no crash in sight.
ci_run_bench_step() {
  local name=$1
  shift
  local status=0
  local restore_errexit=""

  mkdir -p "$CI_BENCH_LOG_DIR"
  # A failing step is data to report, not a reason to abandon the remaining
  # suites, so it must survive an [errexit] (with or without [pipefail]) caller.
  case $- in
  *e*) restore_errexit=1 ;;
  esac
  set +e
  # [tee] is in the pipeline, so [$?] is tee's status; take the step's own.
  "$@" 2>&1 | tee "$CI_BENCH_LOG_DIR/$name.log"
  status=${PIPESTATUS[0]}
  [ -z "$restore_errexit" ] || set -e

  if [ "$status" -ne 0 ]; then
    echo "benchmark step '$name' failed: $(ci_describe_exit_status "$status")"
    CI_FAILED_BENCH_STEPS+=("$name:$status")
  fi
  return 0
}

# Names every failed step with its exit status and replays the tail of each
# failed step's captured log, so the failure lands in the part of the job log
# that survives. Returns 0 when every step passed and 1 otherwise.
ci_report_failed_bench_steps() {
  local restore_xtrace="" entry name status log

  # The replayed logs are only readable if the shell is not tracing them.
  case $- in
  *x*) restore_xtrace=1 ;;
  esac
  { set +x; } 2>/dev/null

  if [ "${#CI_FAILED_BENCH_STEPS[@]}" -eq 0 ]; then
    [ -z "$restore_xtrace" ] || set -x
    return 0
  fi

  for entry in "${CI_FAILED_BENCH_STEPS[@]}"; do
    name=${entry%:*}
    status=${entry##*:}
    echo "FAILED benchmark step '$name': $(ci_describe_exit_status "$status")"
  done

  for entry in "${CI_FAILED_BENCH_STEPS[@]}"; do
    name=${entry%:*}
    log="$CI_BENCH_LOG_DIR/$name.log"
    echo "===== last $CI_BENCH_FAIL_TAIL_LINES lines of $log ====="
    if [ -f "$log" ]; then
      tail -n "$CI_BENCH_FAIL_TAIL_LINES" "$log" || true
    else
      echo "(no captured log at $log)"
    fi
    echo "===== end of '$name' log ====="
  done

  [ -z "$restore_xtrace" ] || set -x
  return 1
}

# Digest of the commands that killed benchmark scripts in this run. The
# combined-metrics schedule replays it at the very end of the job, where it
# stays inside the part of the log GitHub still serves.
CI_BENCH_FAILURE_LOG=${CI_BENCH_FAILURE_LOG:-$CI_BENCH_REPO_ROOT/bench_failures.log}
# Which script invocations have already reported. errtrace puts the ERR trap in
# the subshells bash forks for pipeline components, and those report from a
# different process, so the record of having reported has to live in the
# filesystem. It is a directory of claims rather than a file of keys because
# two components of one pipeline can fail at the same moment and run their
# traps concurrently: a read-then-write of a shared file lets both see it empty
# and both report, while [mkdir] of a claim succeeds for exactly one of them.
CI_BENCH_REPORTED_DIR="${CI_BENCH_FAILURE_LOG}.reported"

ci_enter_cleanup() {
  trap - EXIT INT TERM ERR
  set +e
  set +u
}

# Report the command that aborted a benchmark script.
#
# These scripts run under `set -Eeuo pipefail` with xtrace off -- the schedule's
# `set -x` does not cross the process boundary -- so a script killed by errexit
# exits with a bare status and prints nothing about what failed. Install this
# from a script that sets -E and the failing command, its location, and the call
# stack that reached it are named at the point of failure, and a one-line digest
# is appended to $CI_BENCH_FAILURE_LOG for replay at the end of the job.
ci_enable_error_trace() {
  trap 'ci_report_error_trace "$?" "$BASH_COMMAND"' ERR
}

ci_report_error_trace() {
  local status=$1
  local failed_command=$2
  local depth=${#FUNCNAME[@]}
  local script
  local frame
  local location
  local reported_key claim

  # The ERR trap runs whether or not errexit is set, so it also fires on the
  # failures these scripts deliberately tolerate (`set +e` around a benchmark
  # whose timeout is the expected end, cleanup handlers). Report only the
  # failures that actually end the script.
  case $- in
  *e*) ;;
  *) return 0 ;;
  esac

  script=${BASH_SOURCE[depth - 1]:-$0}
  # Frame 0 is this function; frame 1 ran the failed command.
  location="${BASH_SOURCE[1]:-$script}:${BASH_LINENO[0]}"

  # Errexit ends the script at its first failure, so one invocation has one
  # failure worth reporting. Anything after it is propagation: a function that
  # fails inside a pipeline reports from its own subshell, and the parent shell
  # then reports the pipeline itself, naming its last command rather than the
  # one that failed. Keep the first, most specific report. $$ is the script's
  # pid in its subshells too, so it identifies the invocation across both.
  #
  # Claiming the invocation is a single [mkdir], which either creates the
  # directory or fails because it is already there -- there is no window
  # between deciding to report and recording it. A claim that cannot be made
  # at all (an unwritable digest path) falls through and reports anyway: a
  # failure to record must not turn into a failure to report.
  reported_key="$$ $script"
  claim="$CI_BENCH_REPORTED_DIR/${reported_key//[^A-Za-z0-9._-]/_}"
  mkdir -p "$CI_BENCH_REPORTED_DIR" 2>/dev/null || true
  if ! mkdir "$claim" 2>/dev/null && [ -d "$claim" ]; then
    return 0
  fi

  {
    printf 'FAILED: %s exited with status %s\n' "$script" "$status"
    printf '  command: %s\n' "$failed_command"
    for ((frame = 1; frame < depth; frame++)); do
      printf '  at %s:%s in %s\n' \
        "${BASH_SOURCE[frame]}" "${BASH_LINENO[frame - 1]}" "${FUNCNAME[frame]}"
    done
  } >&2

  # Best effort: a digest that cannot be written must not turn a benchmark
  # failure into a reporting failure.
  printf '%s: exit status %s: %s\n' "$location" "$status" "$failed_command" \
    >>"$CI_BENCH_FAILURE_LOG" 2>/dev/null || true

  return 0
}

# Start a run with no digest, so a file left by an earlier run on the same
# host is never reported as this run's failure.
ci_reset_bench_failures() {
  rm -f "$CI_BENCH_FAILURE_LOG"
  rm -rf "$CI_BENCH_REPORTED_DIR"
}

# Replay the digest. Prints nothing when no script aborted.
ci_report_bench_failures() {
  if [ ! -s "$CI_BENCH_FAILURE_LOG" ]; then
    return 0
  fi

  echo "===== commands that aborted benchmark scripts ====="
  cat "$CI_BENCH_FAILURE_LOG"
  echo "===== end of benchmark failure digest ====="
}

ci_wait_port() {
  CI_WAIT_PORT_TIMEOUT_SEC=$CI_WAIT_PORT_TIMEOUT_SEC \
    "$CI_BENCH_REPO_ROOT/scripts/wait-port.sh" "$@"
}

ci_wait_port_hosts() {
  local port=$1
  local host
  local status=0
  local -a wait_pids=()
  shift

  for host in "$@"; do
    ci_wait_port "$host" "$port" &
    wait_pids+=("$!")
  done
  for wait_pid in "${wait_pids[@]}"; do
    if ! wait "$wait_pid"; then
      status=1
    fi
  done
  return "$status"
}

ci_remote_run() {
  local host=$1
  shift
  ssh "${CI_SSH_OPTS[@]}" "$host" "$@"
}

ci_control_stop_hosts() {
  local control_cli=$1
  local control_port=$2
  local host
  shift 2

  for host in "$@"; do
    "$control_cli" "$host" "$control_port" stop || true
  done
}

ci_control_shutdown_hosts() {
  local control_cli=$1
  local control_port=$2
  local host
  shift 2

  for host in "$@"; do
    "$control_cli" "$host" "$control_port" shutdown || true
  done
}

ci_control_clear_work_dirs() {
  local control_cli=$1
  local control_port=$2
  local host
  shift 2

  for host in "$@"; do
    "$control_cli" "$host" "$control_port" clear_work_dir
  done
}

# How long a reaped process gets to exit on SIGTERM before it is escalated to
# SIGKILL, and how long the SIGKILL then gets to take effect.
CI_REAP_TERM_TIMEOUT_SEC=${CI_REAP_TERM_TIMEOUT_SEC:-20}
CI_REAP_KILL_TIMEOUT_SEC=${CI_REAP_KILL_TIMEOUT_SEC:-10}

# The program [ci_remote_reap] runs on one host to reap [$1] (a space-separated
# list of process names), waiting up to [$2] seconds for SIGTERM to work and
# [$3] more for SIGKILL. Exits 2, naming the survivors, if anything outlives
# both, and 3 if the signals could not be sent at all -- a host whose [killall]
# is broken has not been shown to have a process refusing to die, and
# must not be reported as though it had.
#
# [killall] returns as soon as it has delivered the signal, so reaping with a
# bare [killall <names>] hands the caller back a host where the processes have
# been asked to exit but still hold their listening sockets. The benchmark
# scripts bind fixed ports within seconds of reaping and lose that race: a run
# whose previous hog-mempool was still on its way out dies with
# "bind(10.100.0.110:8075): Address already in use", and one whose previous
# hog-exec was still on its way out dies the same way on 8040. Waiting for the
# processes to actually go is what makes the reap mean what its callers read it
# as meaning.
#
# The liveness check reads [ps] rather than signalling with another [killall]:
# it needs no privilege, and signalling a process to ask whether it is there
# conflates "gone" with "not permitted to signal". Where /proc hides other
# users' processes [ps] sees nothing and this degrades to the old
# fire-and-forget behaviour, which is the floor rather than a regression.
#
# The names are matched literally, with [grep -xF] over [ps -eo comm=], because
# [killall] targets them literally. [pgrep -x] would not do: it anchors its
# operand but still reads it as a regular expression, so the configured
# [chuckd.dev] also matches a [chuckdXdev] that [killall] never signalled --
# and a liveness check that answers "alive" about a process nothing is killing
# waits out both timeouts and then fails the reap.
ci_reap_program() {
  cat <<EOF
procs='$1'
term_timeout=$2
kill_timeout=$3

survivors() {
  found=
  live=\$(ps -eo comm= 2>/dev/null) || live=
  for proc in \$procs; do
    if printf '%s\n' "\$live" | grep -qxF "\$proc"; then
      found="\$found \$proc"
    fi
  done
  printf '%s' "\$found"
}

# Polls for up to \$1 seconds. Returns 0 as soon as nothing is left.
wait_gone() {
  elapsed=0
  while :; do
    [ -n "\$(survivors)" ] || return 0
    [ "\$elapsed" -lt "\$1" ] || return 1
    sleep 1
    elapsed=\$((elapsed + 1))
  done
}

# Sends \$1 to every named process. Returns nonzero only when the signal could
# not be sent at all, which has to be told apart from a process that refused to
# die: [-q] silences killall's own "no process found", so a nonzero exit that
# still says something is killall failing rather than an absence of
# processes to kill.
signal_all() {
  err=\$(killall -q "-\$1" \$procs 2>&1)
  status=\$?
  if [ "\$status" -ne 0 ] && [ -n "\$err" ]; then
    echo "cannot signal processes: \$err" >&2
    return 1
  fi
  return 0
}

# Signal first and ask questions after: a host that will not report its
# processes must still have them reaped.
signal_all TERM || exit 3
wait_gone "\$term_timeout" && exit 0

echo "alive after SIGTERM, escalating to SIGKILL:\$(survivors)" >&2
signal_all KILL || exit 3
wait_gone "\$kill_timeout" && exit 0

echo "alive after SIGKILL:\$(survivors)" >&2
exit 2
EOF
}

# Reaps the named processes on one host and does not return until they are gone.
# Returns nonzero only when a process outlived SIGKILL, which is a stale host
# that needs a human. Anything else -- an unreachable host, a host with no
# [killall] -- is reported and tolerated, because the flaky link to these hosts
# must not be able to fail a benchmark on its own.
ci_remote_reap() {
  local host=$1
  local processes=$2
  local output status=0

  output=$(ci_remote_run "$host" \
    "$(ci_reap_program "$processes" "$CI_REAP_TERM_TIMEOUT_SEC" \
      "$CI_REAP_KILL_TIMEOUT_SEC")" 2>&1) || status=$?
  if [ -n "$output" ]; then
    printf '%s\n' "$output" | sed "s/^/[$host] /" >&2
  fi

  if [ "$status" -eq 2 ]; then
    echo "$host still has benchmark processes that survived SIGKILL" >&2
    return 1
  fi
  if [ "$status" -ne 0 ]; then
    echo "warning: could not reap processes on $host (status $status)" >&2
  fi
  return 0
}

ci_remote_killall_hosts() {
  local processes=$1
  local host
  local reap_pid
  local status=0
  local -a reap_pids=()
  shift

  # A reap now waits out a grace period, so the hosts go concurrently: eight of
  # them serialised behind one would add minutes to every benchmark step.
  for host in "$@"; do
    ci_remote_reap "$host" "$processes" &
    reap_pids+=("$!")
  done
  for reap_pid in "${reap_pids[@]}"; do
    if ! wait "$reap_pid"; then
      status=1
    fi
  done
  return "$status"
}
