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
#
# The optional third argument overrides the timeout in seconds, for a milestone
# that is not a startup milestone and so is not bounded by the readiness
# budget. 0 waits indefinitely.
wait_control_log_line() {
  local wanted_label=$1
  local needle=$2
  local timeout_sec=${3:-${CI_WAIT_PORT_TIMEOUT_SEC:-600}}
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

# The line a mempool wrapper writes to its own log when its run is over. The
# run's exit status, normalized so that the wrapper's own timeout is not a
# failure, follows the prefix.
MEMPOOL_RUN_COMPLETE_PREFIX='MEMPOOL_RUN_COMPLETE status='

# Waits for one mempool's run to end and reports its exit status as this
# function's own. Replays the mempool's log when the run failed, because by then
# every readiness wait has already passed and nothing else has anything to say
# about a mempool that died mid-run. [$2] bounds the wait in seconds; 0 waits
# indefinitely.
#
# A mempool cannot be awaited with [control_cli wait]. That reports on whichever
# process its host started most recently, and hog-db shares a host with the
# mempool and is launched after it -- it has to be, it dials the mempool's db
# RPC server -- so the wait lands on hog-db, which runs until the suite reaps
# it. Nothing ever ends the wait, and the schedule sits at the end of an
# otherwise complete benchmark until its own timeout kills the job, with the
# results uncollected and no failure to point at. Waiting for a line the mempool
# writes itself names the process being awaited whatever else the host has been
# asked to start since.
await_mempool_run() {
  local wanted_label=$1
  local timeout_sec=$2
  local record host label stdout_path stderr_path
  local line status

  if ! wait_control_log_line "$wanted_label" "$MEMPOOL_RUN_COMPLETE_PREFIX" \
    "$timeout_sec"; then
    echo "$wanted_label never reported the end of its run" >&2
    return 1
  fi

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue

    line=$(read_last_control_log_line "$host" "$stderr_path" \
      "$MEMPOOL_RUN_COMPLETE_PREFIX") || line=''
    status=${line##*"$MEMPOOL_RUN_COMPLETE_PREFIX"}
    status=${status%%[!0-9]*}
    if [[ -z $status ]]; then
      echo "could not read $wanted_label exit status from $stderr_path on $host" >&2
      return 1
    fi

    if [[ $status -ne 0 ]]; then
      echo "$wanted_label $(ci_describe_exit_status "$status")" >&2
      replay_control_log_for_label "$wanted_label"
      return "$status"
    fi
    echo "$wanted_label run completed"
    return 0
  done

  echo "no control log record for $wanted_label" >&2
  return 1
}

# The last line of a remote log matching a fixed prefix. Empty output and a
# nonzero status both mean "no such line".
read_last_control_log_line() {
  local host=$1
  local remote_path=$2
  local prefix=$3
  local quoted_prefix quoted_path

  printf -v quoted_prefix '%q' "$prefix"
  printf -v quoted_path '%q' "$remote_path"
  remote_run "$host" \
    "grep -F -- $quoted_prefix $quoted_path | tail -n 1"
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
