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

ci_enter_cleanup() {
  trap - EXIT INT TERM
  set +e
  set +u
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

ci_remote_killall_hosts() {
  local processes=$1
  local host
  shift

  for host in "$@"; do
    ci_remote_run "$host" "sudo -n /usr/bin/killall -q $processes" || true
  done
}
