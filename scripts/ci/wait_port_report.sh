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

# Waits for a process to report its own exit status on a sentinel line, and
# prints the status it reported.
#
# [control_cli wait] cannot be used for this. It waits on whatever the control
# server started most recently on that host, not on the process the caller
# launched, so a script that starts a second, longer-lived server on the same
# host afterwards waits on that one instead -- and if that one blocks forever,
# so does the wait. Have the process report its own status and read it back out
# of its log, which is true no matter what else runs on the host.
#
# The status goes to stdout so the caller can capture it; progress and
# diagnostics go to stderr so they survive that capture.
wait_control_log_status() {
  local wanted_label=$1
  local prefix=$2
  local timeout_sec=${3:-${CI_WAIT_PORT_TIMEOUT_SEC:-600}}
  local record host label stdout_path stderr_path
  local quoted_prefix quoted_stderr_path poll_command output status

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue

    printf -v quoted_prefix '%q' "$prefix"
    printf -v quoted_stderr_path '%q' "$stderr_path"
    printf -v poll_command 'timeout %ss bash -c %q' "$timeout_sec" \
      "until line=\$(grep -F -- $quoted_prefix $quoted_stderr_path | tail -n 1); [ -n \"\$line\" ]; do sleep 0.1; done; printf '%s\\n' \"\$line\""

    echo "Waiting for $wanted_label to report: ${prefix}<status>" >&2
    if ! output=$(remote_run "$host" "$poll_command"); then
      echo "Timed out after ${timeout_sec}s waiting for $wanted_label to report its exit status" >&2
      replay_control_log "$host" "$label" "$stdout_path" "$stderr_path" >&2
      return 1
    fi

    # Keep the digits that follow the last sentinel in the log: a run that
    # reused this file must be read as the run that wrote last.
    status=${output##*"$prefix"}
    status=${status%%[!0-9]*}
    if [[ -z $status ]]; then
      echo "$wanted_label reported an unreadable exit status: $output" >&2
      replay_control_log "$host" "$label" "$stdout_path" "$stderr_path" >&2
      return 1
    fi

    echo "$wanted_label reported: ${prefix}${status}" >&2
    printf '%s\n' "$status"
    return 0
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
