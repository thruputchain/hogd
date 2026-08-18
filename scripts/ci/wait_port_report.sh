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

# How much of a log's end a marker wait reads, and how often it looks.
CONTROL_LOG_MARKER_TAIL_LINES=20
CONTROL_LOG_MARKER_POLL_SEC=2

# Waits for a process to write [marker] to its stderr as it exits, and prints
# whatever follows the marker on that line. Progress goes to stderr, so the
# marker's value is all this puts on stdout.
#
# This is how a process reports its own outcome to the script that launched it.
# [control_cli wait] cannot: the control server answers a wait with the state
# of the most recently started process on the host it is asked about, so it
# says nothing about a specific process once a second one has been started
# alongside it -- it reports whichever of them was started last.
#
# Unlike a readiness wait, this one runs for as long as the process it is
# waiting on, against a log still being written at benchmark rates. It reads
# only the end of that log, and slowly: re-reading all of it every tenth of a
# second would put the wait's own I/O on the host being measured. Reading the
# end is enough because the marker is written after the process it describes
# has exited, so nothing follows it.
#
# [timeout_sec] is passed to [timeout], so 0 waits without a deadline -- for a
# run whose whole point is to continue until it is stopped by hand.
wait_control_log_marker() {
  local wanted_label=$1
  local marker=$2
  local timeout_sec=${3:-${CI_WAIT_PORT_TIMEOUT_SEC:-600}}
  local record host label stdout_path stderr_path
  local quoted_marker quoted_stderr_path read_tail poll_body poll_command line

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue

    printf -v quoted_marker '%q' "$marker"
    printf -v quoted_stderr_path '%q' "$stderr_path"
    # The log does not exist until the process writes to it, which is not yet a
    # failure to report -- keep polling rather than letting [tail] say so.
    read_tail="tail -n $CONTROL_LOG_MARKER_TAIL_LINES $quoted_stderr_path 2>/dev/null"
    poll_body="until $read_tail | grep -Fq -- $quoted_marker;"
    poll_body+=" do sleep $CONTROL_LOG_MARKER_POLL_SEC; done;"
    poll_body+=" $read_tail | grep -F -- $quoted_marker | tail -n 1"
    printf -v poll_command 'timeout %ss bash -c %q' "$timeout_sec" "$poll_body"

    echo "Waiting for $wanted_label to report: $marker" >&2
    if ! line=$(remote_run "$host" "$poll_command"); then
      echo "Timed out after ${timeout_sec}s waiting for $wanted_label to report: $marker" >&2
      replay_control_log "$host" "$label" "$stdout_path" "$stderr_path"
      return 1
    fi
    if [[ $line != *"$marker"* ]]; then
      echo "$wanted_label reported an unreadable [$marker] line: [$line]" >&2
      return 1
    fi
    echo "$wanted_label reported: $line" >&2
    printf '%s\n' "${line##*"$marker"}"
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
