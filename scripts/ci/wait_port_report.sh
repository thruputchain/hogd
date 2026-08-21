#!/usr/bin/env bash

# A readiness wait that says what was supposed to be listening. Intended to be
# sourced by a benchmark script that has [CONTROL_LOG_RECORDS], [hog_ip] and
# [remote_run] in scope.
#
# [control_cli start] returns as soon as a process is spawned, so a process that
# dies during startup is not reported by its launch. The run fails minutes later
# in a readiness wait, where [wait-port.sh] prints only
#
#     Timed out waiting for 10.100.0.105:8040 after 600s
#
# and the script exits 1. Why nothing is listening is in the process's own log,
# which lives on the remote host and is otherwise collected only at the end of
# the run. Replay it here instead, at the point of failure.

READINESS_LOG_TAIL_LINES=80

# Drop-in for [ci_wait_port]: same arguments, same status, plus the logs of
# everything launched on that host when the wait times out.
wait_port_or_report() {
  local ip=$1
  local port=$2
  local record host label stdout_path stderr_path quoted_paths

  if ci_wait_port "$ip" "$port"; then
    return 0
  fi

  echo "readiness: nothing is listening on $ip:$port" >&2
  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $(hog_ip "$host") == "$ip" ]] || continue
    replay_control_log "$host" "$label" "$stdout_path" "$stderr_path"
  done
  return 1
}

# Waits for a process to report a startup milestone without opening a probe
# connection to its RPC server. Some RPC clients may connect immediately after
# the listener is published, so a connect-and-close probe can race real traffic.
wait_control_log_line() {
  local wanted_label=$1
  local needle=$2
  local timeout_sec=${CI_WAIT_PORT_TIMEOUT_SEC:-600}
  local record host label stdout_path stderr_path
  local quoted_needle quoted_stderr_path poll_command

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue

    printf -v quoted_needle '%q' "$needle"
    printf -v quoted_stderr_path '%q' "$stderr_path"
    printf -v poll_command 'timeout %ss bash -c %q' "$timeout_sec" \
      "until grep -Fq -- $quoted_needle $quoted_stderr_path; do sleep 0.1; done"

    echo "Waiting for $wanted_label to report: $needle"
    if remote_run "$host" "$poll_command"; then
      echo "$wanted_label reported: $needle"
      return 0
    fi

    echo "Timed out after ${timeout_sec}s waiting for $wanted_label to report: $needle" >&2
    replay_control_log "$host" "$label" "$stdout_path" "$stderr_path"
    return 1
  done

  echo "no control log record for $wanted_label" >&2
  return 1
}

# How long a launched process gets to record its exit status before the wait
# gives up on it.
CONTROL_EXIT_WAIT_TIMEOUT_SEC=${CONTROL_EXIT_WAIT_TIMEOUT_SEC:-900}

# Waits for one launched process to exit and returns the status it exited with.
#
# [control_cli wait] cannot answer this question. The control server's
# proc-status RPC reports the host's *most recently launched* process, so a wait
# aimed at one process silently becomes a wait on whatever was launched on that
# host after it. On the mempool hosts that is hog-db, which outlives the
# benchmark: the wait then never returns and the whole schedule sits idle until
# its timeout kills it.
#
# The process records its own status in [$marker_path] instead, and this reads
# it. A marker file also removes the race a poll has: the status is still there
# to be read when the wait starts after the process has already gone.
#
# Returns the recorded status. A wait that could not observe the exit at all --
# an unreachable host, a marker that never appeared, a marker holding something
# other than a status -- says so and returns nonzero, so a run is never reported
# as having exited cleanly on the strength of a wait that did not happen.
wait_control_exit_status() {
  local wanted_label=$1
  local marker_path=$2
  local timeout_sec=${CONTROL_EXIT_WAIT_TIMEOUT_SEC}
  local record host label stdout_path stderr_path
  local quoted_marker poll_command output recorded status=0

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue

    printf -v quoted_marker '%q' "$marker_path"
    printf -v poll_command 'timeout %ss bash -c %q' "$timeout_sec" \
      "until [ -f $quoted_marker ]; do sleep 1; done; read -r status <$quoted_marker; printf 'CONTROL_EXIT_STATUS=%s\\n' \"\$status\""

    echo "Waiting for $wanted_label on $host to exit"
    output=$(remote_run "$host" "$poll_command") || status=$?
    if [[ $status -ne 0 ]]; then
      if [[ $status -eq 124 ]]; then
        echo "Timed out after ${timeout_sec}s waiting for $wanted_label on $host to exit" >&2
      else
        echo "could not wait for $wanted_label on $host to exit (status $status)" >&2
      fi
      return "$status"
    fi

    recorded=$(printf '%s\n' "$output" | sed -n 's/^CONTROL_EXIT_STATUS=//p' | tail -n 1)
    if [[ ! $recorded =~ ^[0-9]+$ ]]; then
      echo "$wanted_label on $host recorded no usable exit status in $marker_path" >&2
      printf '%s\n' "$output" | sed 's/^/     | /' >&2
      return 1
    fi

    echo "$wanted_label on $host exited with status $recorded"
    return "$recorded"
  done

  echo "no control log record for $wanted_label" >&2
  return 1
}

# Replays the tail of one launched process's stdout and stderr. Best effort: a
# host that has gone away must not replace the failure being reported with a
# failure of its own.
replay_control_log() {
  local host=$1
  local label=$2
  local stdout_path=$3
  local stderr_path=$4
  local quoted_paths

  echo "===== last $READINESS_LOG_TAIL_LINES lines of $label on $host ====="
  printf -v quoted_paths '%q %q' "$stdout_path" "$stderr_path"
  remote_run "$host" "tail -n $READINESS_LOG_TAIL_LINES $quoted_paths" || true
}

# Replays the logs of a process by the label it was launched under. Used when a
# process that came up cleanly dies later, where a readiness wait has nothing
# left to say.
replay_control_log_for_label() {
  local wanted_label=$1
  local record host label stdout_path stderr_path

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue
    replay_control_log "$host" "$label" "$stdout_path" "$stderr_path"
    return 0
  done

  echo "no control log record for $wanted_label" >&2
}
