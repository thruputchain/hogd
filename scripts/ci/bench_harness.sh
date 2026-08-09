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

# How long to wait for a stale process to be gone, first after SIGTERM and
# then after SIGKILL, before giving up on it.
CI_KILLALL_WAIT_SEC=${CI_KILLALL_WAIT_SEC:-15}

# Absolute, because the benchmark hosts grant this one command through sudo by
# path.
CI_KILLALL_BIN=${CI_KILLALL_BIN:-/usr/bin/killall}

# The remote half of [ci_remote_killall_hosts]: signal every named process and
# wait for it to actually be gone, escalating to SIGKILL if SIGTERM was not
# enough.
#
# [killall] returns as soon as it has delivered the signals, so a process that
# takes its time shutting down -- or that never comes down at all -- is still
# holding its listening ports when the next benchmark suite launches its own
# processes on the same host. That collision does not surface here. It surfaces
# minutes later, inside a suite that did nothing wrong, as
#
#     bind(10.100.0.105:8040): Address already in use
#
# from the suite whose ports the leftovers happen to occupy: [mempool_bench]
# runs a [hog-exec] on hog5 with ports 8040 and 8041, which is exactly what
# [mempool_multi_node_bench] gives its two hog5 executor slots when it runs
# next. Wait for the processes here, where the stale one can still be named.
ci_killall_script() {
  local processes=$1

  cat <<EOF
set -u

procs='$processes'
wait_sec='$CI_KILLALL_WAIT_SEC'
killall='$CI_KILLALL_BIN'

# A kill that cannot be confirmed is the failure this script exists to prevent,
# so a host without the tool to confirm it must say so rather than report the
# processes reaped.
command -v pgrep >/dev/null 2>&1 || {
  echo 'pgrep not found (install procps)' >&2
  exit 1
}

# Names the processes from [procs] that are still running. Probing with [pgrep]
# rather than a signal-0 [killall] keeps the answer independent of whether this
# shell is allowed to signal the process: a probe that fails for lack of
# privilege would report a stale process as gone, which is the one answer that
# must never be wrong here.
alive_procs() {
  found=''
  for p in \$procs; do
    if pgrep -x "\$p" >/dev/null 2>&1; then
      found="\$found \$p"
    fi
  done
  printf '%s' "\${found# }"
}

# Waits out [wait_sec] for the named processes to exit and names whatever is
# left.
wait_for_exit() {
  waited=0
  while [ "\$waited" -lt "\$wait_sec" ] && [ -n "\$(alive_procs)" ]; do
    sleep 1
    waited=\$((waited + 1))
  done
  alive_procs
}

# [killall] reports failure when any of the names it was given matched nothing,
# which is the normal case for a host that runs only some of these processes.
sudo -n "\$killall" -q \$procs || true

remaining=\$(wait_for_exit)
[ -n "\$remaining" ] || exit 0

echo "stale processes still running \${wait_sec}s after SIGTERM: \$remaining" >&2
sudo -n "\$killall" -q -KILL \$remaining || true

remaining=\$(wait_for_exit)
[ -n "\$remaining" ] || exit 0

echo "stale processes survived SIGKILL: \$remaining" >&2
exit 1
EOF
}

# Reaps the named processes on each host. Returns non-zero, naming the host,
# when a host could not be reached or still has one of them running: a suite
# that starts anyway fails later on a port it cannot bind, with nothing left in
# the log to say why.
ci_remote_killall_hosts() {
  local processes=$1
  local host
  local status=0
  local script
  shift

  script=$(ci_killall_script "$processes")
  for host in "$@"; do
    if ! ci_remote_run "$host" "$script"; then
      echo "failed to reap stale benchmark processes on $host" >&2
      status=1
    fi
  done
  return "$status"
}
