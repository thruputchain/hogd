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

# Waits for a process to announce something in its stderr log and prints what
# it announced: the rest of the line after [marker]. Where
# [wait_control_log_line] watches for the start of a process, this watches for
# the end of one.
#
# [control_cli wait] cannot be used for that. The control server answers status
# queries about the process it started most recently, so on a host that runs
# more than one service the wait reports on whichever service was launched
# last, not on the process whose exit ends the run. A wait aimed at a service
# that runs until teardown never returns at all.
#
# The wait polls from here rather than holding one ssh session open for the
# length of the run: a session left idle that long is not guaranteed to
# survive, and a connection dropped mid-run would be indistinguishable from a
# process that never reported. [timeout_sec] of 0 waits indefinitely. Progress
# and diagnostics go to stderr, so that stdout carries only the reported value.
wait_control_log_marker() {
  local wanted_label=$1
  local marker=$2
  local timeout_sec=${3:-0}
  local poll_interval_sec=${4:-5}
  local record host label stdout_path stderr_path
  local quoted_marker quoted_stderr_path line deadline=0

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue

    printf -v quoted_marker '%q' "$marker"
    printf -v quoted_stderr_path '%q' "$stderr_path"
    # Elapsed time rather than a count of polls: a poll that itself takes a
    # while -- an ssh to a host that has gone away -- must still count against
    # the deadline.
    [ "$timeout_sec" -eq 0 ] || deadline=$((SECONDS + timeout_sec))

    echo "Waiting for $wanted_label to report: $marker" >&2
    while :; do
      # An unreachable host and a marker that has not been printed yet are the
      # same thing here: nothing to report yet, keep waiting until the timeout.
      line=$(remote_run "$host" \
        "grep -F -- $quoted_marker $quoted_stderr_path | tail -n 1") || line=''
      if [[ -n $line ]]; then
        echo "$wanted_label reported: $line" >&2
        printf '%s\n' "${line##*"$marker"}"
        return 0
      fi

      if [ "$deadline" -ne 0 ] && [ "$SECONDS" -ge "$deadline" ]; then
        echo "Timed out after ${timeout_sec}s waiting for $wanted_label to report: $marker" >&2
        replay_control_log "$host" "$label" "$stdout_path" "$stderr_path" >&2
        return 1
      fi

      sleep "$poll_interval_sec"
    done
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
