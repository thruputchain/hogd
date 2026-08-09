#!/usr/bin/env bash

# A readiness wait that says what was supposed to be listening. Intended to be
# sourced by a benchmark script that has [CONTROL_LOG_RECORDS], [hog_ip],
# [remote_run], [CONTROL_CLI] and [CI_CONTROL_PORT] in scope.
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

# How long a slice of a readiness wait runs before the host is asked whether the
# process that was supposed to come up has already exited.
#
# A wait whose process is dead is a wait that can only end in its timeout, and
# the timeout is ten minutes. In the run that prompted this, both hog-exec
# shards on hog5 died a second after launch on "Address already in use"; the
# suite waited the full 600s anyway and then reported the metrics port, which
# was neither the port that failed to bind nor a port anything was wrong with.
# Checking between slices ends that wait about as soon as it becomes hopeless,
# and the log replay below then lands on the process that actually died.
READINESS_PROC_CHECK_INTERVAL_SEC=${READINESS_PROC_CHECK_INTERVAL_SEC:-30}
# How long that question may take before it is abandoned. [control_cli] retries
# a connection that is refused, so a control server that has gone away would
# otherwise stall the readiness wait this is meant to cut short.
READINESS_PROC_CHECK_TIMEOUT_SEC=${READINESS_PROC_CHECK_TIMEOUT_SEC:-20}

# Drop-in for [ci_wait_port]: same arguments, same status, plus the logs of
# everything launched on that host when the wait ends without the port.
#
# The wait is served in slices, so the "(timeout: Ns)" line [wait-port.sh]
# prints is now the slice rather than the whole budget. The budget itself is
# unchanged and covers everything done under it, the checks between slices
# included: a port that never comes up is waited on for
# [CI_WAIT_PORT_TIMEOUT_SEC] and no longer, however the time inside that is
# spent.
wait_port_or_report() {
  local ip=$1
  local port=$2
  local budget slice host reason remaining probe_started answered
  local waited=0
  local -a hosts=()

  # [ci_wait_port] reads the same variable, and 0 means "wait forever" to it.
  budget=${CI_WAIT_PORT_TIMEOUT_SEC:-60}
  mapfile -t hosts < <(hosts_with_control_logs_for_ip "$ip")

  while :; do
    slice=$READINESS_PROC_CHECK_INTERVAL_SEC
    # A budget of 0 has no remainder to trim the last slice against.
    if [ "$budget" -ne 0 ] && [ "$((budget - waited))" -lt "$slice" ]; then
      slice=$((budget - waited))
    fi

    if CI_WAIT_PORT_TIMEOUT_SEC=$slice ci_wait_port "$ip" "$port"; then
      return 0
    fi
    # [wait-port.sh] fails only by running out its timeout, so a slice that came
    # back without the port took the whole slice.
    waited=$((waited + slice))

    for host in ${hosts[@]+"${hosts[@]}"}; do
      # 0 is "no cap of its own" to [launched_process_exited], which is what an
      # unbounded budget leaves it with.
      remaining=0
      if [ "$budget" -ne 0 ]; then
        remaining=$((budget - waited))
        [ "$remaining" -gt 0 ] || break
      fi

      probe_started=$SECONDS
      answered=1
      reason=$(launched_process_exited "$host" "$remaining") || answered=0
      # A probe is time the caller spent waiting whether or not it answered, so
      # charge it. Left uncharged, a wedged control server -- which is exactly
      # what an unanswered probe suggests -- would add its timeout to every
      # slice and carry the wait well past the budget it was given.
      waited=$((waited + SECONDS - probe_started))
      [ "$answered" -eq 1 ] || continue

      echo "readiness: giving up on $ip:$port;" \
        "$(last_control_log_label_for_host "$host") on $host has already" \
        "exited ($reason), so this suite cannot come up whatever happens on" \
        "this port" >&2
      replay_control_logs_for_ip "$ip"
      return 1
    done

    if [ "$budget" -ne 0 ] && [ "$waited" -ge "$budget" ]; then
      break
    fi
  done

  echo "readiness: nothing is listening on $ip:$port" >&2
  replay_control_logs_for_ip "$ip"
  return 1
}

# What the control server on a host says about the last process it launched,
# when that process has already exited. Prints the reason and returns 0 only
# then; every other outcome is "no answer", which is not the same as "still
# running" and must not end a wait that could still succeed.
#
# [expect_running] exits 1 for a process that has exited, for a server it could
# not reach, for a version mismatch, and for a query made before anything was
# launched. Only the first is grounds for giving up on a port, and the four are
# told apart by what the command printed rather than by its status.
#
# The server answers for the most recent process it launched and for no other,
# so this is a verdict on the host rather than on the port being waited for. A
# host runs more than one -- hog5 runs both hog-exec shards -- and the answer
# cuts both ways:
#
#   - a launch that died is not reported while a later one on the same host is
#     still running, which only costs a longer wait; and
#   - the last launch dying ends the waits for the earlier ones too, even where
#     one of those could still have bound its port.
#
# The second is deliberate. Everything this suite launches is a server that has
# to stay up for the whole run, and the script has no way to restart one, so a
# host that has lost any of them cannot produce a passing run -- an earlier
# launch going on to bind its port would only move the failure to the next wait,
# which is the ten-minute stall this exists to remove. What the second case must
# not do is misattribute: the report names the launch the server answered for,
# and every process on the host is replayed, so the one that actually died is in
# the output whether or not it owns the port that was being waited on.
#
# $2 caps the probe at what is left of the caller's budget, with 0 meaning it
# has none of its own to spend against.
launched_process_exited() {
  local host=$1
  local limit=${2:-0}
  local output reason
  local probe_timeout=$READINESS_PROC_CHECK_TIMEOUT_SEC

  [ -n "${CONTROL_CLI:-}" ] && [ -n "${CI_CONTROL_PORT:-}" ] || return 1
  if [ "$limit" -ne 0 ] && [ "$limit" -lt "$probe_timeout" ]; then
    probe_timeout=$limit
  fi

  output=$(timeout "$probe_timeout" \
    "$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" expect_running 2>&1) || true
  [[ $output == *"Process is not running: "* ]] || return 1

  reason=$(printf '%s\n' "$output" | sed -n 's/.*Process is not running: //p')
  printf '%s' "${reason%%$'\n'*}"
}

# The hosts something was launched on that answer to this address, in launch
# order and without repeats. A host runs more than one process -- hog5 runs both
# hog-exec shards -- and is only worth one question about its liveness.
hosts_with_control_logs_for_ip() {
  local ip=$1
  local record host label stdout_path stderr_path
  local -a seen=()

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $(hog_ip "$host") == "$ip" ]] || continue
    [[ " ${seen[*]-} " == *" $host "* ]] && continue
    seen+=("$host")
  done

  [ "${#seen[@]}" -eq 0 ] || printf '%s\n' "${seen[@]}"
}

# The label of the last thing launched on a host, which is the launch the
# control server answers about. Naming it keeps a host-level verdict from
# reading as one about the port: on hog5 the answer is about
# [node0_hog_exec_slot_b] whether the wait that asked was for slot B's port or
# slot A's.
last_control_log_label_for_host() {
  local wanted_host=$1
  local record host label stdout_path stderr_path
  local found='the last process launched'

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $host == "$wanted_host" ]] || continue
    found=$label
  done

  printf '%s' "$found"
}

# Replays every process launched on the host behind an address. What is wrong
# with a port is in the log of whatever was supposed to be listening on it, and
# which of a host's processes that is cannot be told from the port alone.
replay_control_logs_for_ip() {
  local ip=$1
  local record host label stdout_path stderr_path

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $(hog_ip "$host") == "$ip" ]] || continue
    replay_control_log "$host" "$label" "$stdout_path" "$stderr_path"
  done
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
