#!/usr/bin/env bash

# Shared helpers for CI benchmark scripts. This file is intended to be sourced.

CI_BENCH_HARNESS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CI_BENCH_REPO_ROOT=$(cd "$CI_BENCH_HARNESS_DIR/../.." && pwd)

CI_WAIT_PORT_TIMEOUT_SEC=${CI_WAIT_PORT_TIMEOUT_SEC:-60}
CI_SSH_OPTS=(-n -T -o BatchMode=yes -o ConnectTimeout=5 -o VisualHostKey=no)

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

# Digest of the commands that killed benchmark scripts, replayed by the
# combined-metrics schedule at the end of its job. That job runs five suites
# into one log and only the tail of it survives, so a suite that fails early is
# otherwise anonymous: the suites run with xtrace off, so errexit kills them
# with a bare exit status and no message. Sourcing installs the trap; the
# suites all set -E, so it reaches their functions too.
CI_BENCH_FAILURE_LOG=${CI_BENCH_FAILURE_LOG:-$CI_BENCH_REPO_ROOT/bench_failures.log}

ci_report_error_trace() {
  local script=${BASH_SOURCE[${#FUNCNAME[@]} - 1]:-$0}
  local at report frame i

  # The ERR trap runs whether or not errexit is set, so ignore the failures the
  # suites tolerate: a benchmark whose timeout is its expected end, cleanup
  # handlers. Then keep only the first report per script -- errexit ends it at
  # its first failure, and what follows is propagation, such as the parent
  # shell reporting a pipeline after the failing component's subshell already
  # reported the command that failed.
  case $- in
  *e*) ;;
  *) return 0 ;;
  esac
  grep -qsF -- "$script: " "$CI_BENCH_FAILURE_LOG" && return 0

  at="${BASH_SOURCE[1]}:${BASH_LINENO[0]}"
  [ "${BASH_SOURCE[1]}" != "$script" ] || at="line ${BASH_LINENO[0]}"
  report="$script: $at: exit status $1: $2"

  # Then the call stack that reached the failure. A command that dies inside a
  # helper reports that helper's line, which is shared by every call site: all
  # ~30 readiness waits in [mempool_multi_node_bench.sh] go through the one
  # [wait-port.sh] invocation in [ci_wait_port], so the innermost location
  # identifies none of them. The frame that names the wait -- which port, and
  # whether it was the [ci_wait_port_hosts] gate or a per-process wait -- is
  # the one back in the suite script. [BASH_LINENO[i]] is where [FUNCNAME[i]]
  # was called, in the file [BASH_SOURCE[i + 1]]; the outermost frame is the
  # script itself and has no call site of its own, so stop short of it.
  for ((i = 1; i < ${#FUNCNAME[@]} - 1; i++)); do
    frame="${BASH_SOURCE[i + 1]}:${BASH_LINENO[i]}"
    [ "${BASH_SOURCE[i + 1]}" != "$script" ] || frame="$script:${BASH_LINENO[i]}"
    report="$report"$'\n'"  in ${FUNCNAME[i]} at $frame"
  done

  echo "FAILED $report" >&2
  echo "$report" >>"$CI_BENCH_FAILURE_LOG" 2>/dev/null || true
}
trap 'ci_report_error_trace "$?" "$BASH_COMMAND"' ERR
