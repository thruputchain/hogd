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

# How long a port may keep answering after the stale-process kill before the
# host is called still busy. A listening socket goes away with its process, so
# this is slack for a process that is on its way out, not a retry budget.
CI_PORT_FREE_WAIT_SEC=${CI_PORT_FREE_WAIT_SEC:-15}
# How many listening sockets [ci_report_port_holders] prints per host.
CI_PORT_HOLDER_REPORT_LINES=${CI_PORT_HOLDER_REPORT_LINES:-40}

# Is anything listening on this address? The same probe [ci_wait_port] waits on,
# which is the point: a port that answers here is a port that would satisfy a
# readiness wait.
ci_port_in_use() {
  nc -z "$1" "$2" 2>/dev/null
}

# The program that asks a host which of the given ports it is listening on,
# emitted as text so it can be handed to ssh and exercised by the self-test.
#
# [ss | grep | head] would return the status of [head], so a host where [ss]
# cannot run comes back successful with no output -- indistinguishable from a
# host with nothing listening, which is the one answer this must never invent.
# Take [ss]'s own status first, on its own, and match against what it printed.
ci_listening_sockets_script() {
  local alternation=$1
  local limit=$2

  cat <<EOF
ci_sockets=\$(ss -lntp 2>/dev/null) || exit 3
printf '%s\n' "\$ci_sockets" | grep -E ':($alternation)\b' | head -n $limit
EOF
}

# Best effort: names what is holding the ports, so the report says which stale
# process to chase rather than only that the host is busy. A host that cannot be
# asked must not replace the failure being reported with a failure of its own.
ci_report_port_holders() {
  local host=$1
  local alternation listeners
  shift

  printf -v alternation '%s|' "$@"
  alternation=${alternation%|}

  echo "===== listening sockets on $host holding $* ====="
  # [ss] names the process only for sockets the caller may see, so a port held
  # by another account matches on the address alone. Both outcomes say more than
  # silence, and neither is worth failing over.
  if ! listeners=$(ci_remote_run "$host" \
    "$(ci_listening_sockets_script "$alternation" "$CI_PORT_HOLDER_REPORT_LINES")"); then
    echo "(could not ask $host what is listening)"
  elif [ -z "$listeners" ]; then
    echo "(nothing found; the port may be held by a process ss did not show)"
  else
    printf '%s\n' "$listeners"
  fi
  echo "===== end of listening sockets on $host ====="
}

# Requires that every port a suite is about to bind on one host is free, and
# names the ones that are not.
#
# A readiness wait is satisfied by whatever answers on the port, including a
# process this run did not start. When the previous suite's processes are still
# up, this run's binaries die on "bind(<ip>:<port>): Address already in use"
# while the stale ones keep answering, so every readiness wait for a port they
# hold passes. The run then fails a readiness timeout later, on the first port
# nothing happened to be holding -- a port and a host that have nothing to do
# with the machine that is actually busy.
#
# Checking before anything launches turns that into the true statement, at the
# moment it becomes true, about the port that is actually taken. $1 is the host
# to ask what is listening, $2 the address the suite binds on it (they differ:
# services bind the benchmark network, ssh reaches the host by name), and the
# rest are its ports.
ci_require_ports_free() {
  local host=$1
  local ip=$2
  local port waited=0
  local -a pending busy
  shift 2

  pending=("$@")
  while :; do
    busy=()
    for port in ${pending[@]+"${pending[@]}"}; do
      if ci_port_in_use "$ip" "$port"; then
        busy+=("$port")
      fi
    done
    if [ "${#busy[@]}" -eq 0 ]; then
      return 0
    fi
    # One deadline for the host, not one per port: a suite binds dozens of them
    # and waiting out each in turn would be minutes of nothing happening.
    if [ "$waited" -ge "$CI_PORT_FREE_WAIT_SEC" ]; then
      break
    fi
    pending=("${busy[@]}")
    sleep 1
    waited=$((waited + 1))
  done

  for port in "${busy[@]}"; do
    echo "preflight: $ip:$port is in use before this run launched anything" >&2
  done
  ci_report_port_holders "$host" "${busy[@]}"
  return 1
}

# [ci_require_ports_free] for ports the coordinator can only be seen to hold
# from the host itself.
#
# A probe from here reaches one address, and a suite binds more than that: a
# loopback-only listener is invisible from another machine, and a port whose
# bind address is not spelled out on the command line cannot be probed with any
# confidence from off-host. So ask the host what it is listening on instead,
# which covers every local address at once, and let a port nothing waits on be
# checked all the same -- a stale process holding it aborts this run's process
# just as surely as one holding a port with a readiness wait behind it.
#
# Not being able to ask is a warning, not a failure: the check exists to explain
# a launch that is about to go wrong, and it must not become a new way for the
# run to go wrong on its own.
ci_require_host_ports_free() {
  local host=$1
  local alternation listeners busy waited=0 status
  shift

  printf -v alternation '%s|' "$@"
  alternation=${alternation%|}

  while :; do
    status=0
    listeners=$(ci_remote_run "$host" \
      "$(ci_listening_sockets_script "$alternation" \
        "$CI_PORT_HOLDER_REPORT_LINES")") || status=$?

    # Unreachable, or reachable but unable to enumerate its listeners. Either
    # way the host did not answer the question, which is not the same as
    # answering "nothing".
    if [ "$status" -ne 0 ]; then
      echo "preflight: could not ask $host what it is listening on;" \
        "leaving $* unchecked" >&2
      return 0
    fi
    if [ -z "$listeners" ]; then
      return 0
    fi
    if [ "$waited" -ge "$CI_PORT_FREE_WAIT_SEC" ]; then
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done

  busy=$(printf '%s\n' "$listeners" | grep -oE ":($alternation)\b" |
    tr -d ':' | sort -u | tr '\n' ' ')
  echo "preflight: $host is listening on ${busy% } before this run launched" \
    "anything" >&2
  echo "===== listening sockets on $host holding ${busy% } ====="
  printf '%s\n' "$listeners"
  echo "===== end of listening sockets on $host ====="
  return 1
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

ci_remote_killall_hosts() {
  local processes=$1
  local host
  shift

  for host in "$@"; do
    ci_remote_run "$host" "sudo -n /usr/bin/killall -q $processes" || true
  done
}
