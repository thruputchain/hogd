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
#
# The same applies to a process that comes up, passes every readiness wait, and
# then dies from a fatal signal mid-run: see
# [replay_crash_backtrace_for_label].

READINESS_LOG_TAIL_LINES=80

# How much of the remote backtrace to pull back. What matters is the top frames
# of the crashing thread, which gdb prints first, so a cap that truncates the
# tail of a 128-thread dump costs nothing and keeps the failure digest readable.
CRASH_BACKTRACE_MAX_LINES=${CRASH_BACKTRACE_MAX_LINES:-200}

# Where to look for the core on the crashed process's host, most likely first.
# [control_server] does not chdir the processes it forks, so they inherit its
# working directory: the job directory, which is [$REPO_ROOT] on every bench
# host. The remote host's home directory is searched after those, by the
# fragment itself, since only the remote shell knows what it is.
if [ -z "${CRASH_CORE_SEARCH_DIRS[*]+set}" ]; then
  declare -a CRASH_CORE_SEARCH_DIRS=("${REPO_ROOT:-.}" /tmp)
fi

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

# The shell fragment that turns a core file into a backtrace, run on the host
# that produced it. The search directories arrive as positional parameters and
# the line cap as an assignment, so the body itself stays single-quoted and
# nothing in it is expanded here on the way through ssh.
#
# Usage: crash_backtrace_remote_script <max lines> <search dir>...
crash_backtrace_remote_script() {
  local max_lines=$1
  local quoted_dirs
  shift

  printf -v quoted_dirs '%q ' "$@"
  printf 'CRASH_MAX_LINES=%s\nset -- %s\n%s\n' \
    "$max_lines" "${quoted_dirs% }" '
# The kernel names the core after kernel.core_pattern, which is a bare "core"
# on some hosts and "core.<pid>" on others, and a stale core from an earlier
# run can sit beside this one, so take the newest match in the first directory
# that has one. "$HOME" comes last because only this shell knows what it is.
core=
for dir in "$@" "$HOME"; do
  [ -d "$dir" ] || continue
  found=$(cd "$dir" && ls -1t core core.* *.core 2>/dev/null | head -n 1)
  if [ -n "$found" ]; then
    core=$dir/$found
    break
  fi
done

if [ -z "$core" ]; then
  pattern=$(cat /proc/sys/kernel/core_pattern 2>/dev/null || echo unknown)
  echo "no core file under $* or $HOME (kernel.core_pattern=$pattern)"
  # A pattern beginning with "|" hands the core to a helper rather than writing
  # a file. Where that helper is systemd-coredump, the stack trace is already
  # in the journal.
  if command -v coredumpctl >/dev/null 2>&1; then
    echo "most recent core dump known to coredumpctl:"
    coredumpctl info --no-pager 2>&1 | head -n "$CRASH_MAX_LINES"
  fi
  exit 0
fi

echo "core file: $core"
if ! command -v gdb >/dev/null 2>&1; then
  echo "(gdb is not installed on this host; the core is left in place)"
  exit 0
fi

# The core records the path the process was launched from, which is how
# [save_core_dumps.sh] pairs a core with its binary. Without the binary gdb
# still unwinds, but the frames have no symbols.
exe=$(gdb -batch -q -c "$core" -ex "info auxv" 2>/dev/null |
  sed -n "s/.*AT_EXECFN[^\"]*\"\(.*\)\"$/\1/p")
# AT_EXECFN is the path as the process was invoked, so it is relative when the
# process was started that way. It is relative to the directory the process ran
# in, which is the directory the kernel wrote the core into.
case $exe in
/* | "") ;;
*) exe=${core%/*}/${exe#./} ;;
esac
if [ -z "$exe" ] || [ ! -f "$exe" ]; then
  echo "(could not identify the binary for $core; frames will be unsymbolized)"
  exe=
fi

# [bt] first: gdb selects the thread that took the signal, so the crash itself
# is at the top of the output and survives the line cap.
gdb -batch -q -ex "set pagination off" -ex bt -ex "thread apply all bt" \
  ${exe:+"$exe"} -c "$core" 2>&1 | head -n "$CRASH_MAX_LINES"
exit 0
'
}

# Retrieves a backtrace for a process that died from a fatal signal. A process
# that comes up cleanly and then crashes leaves a core on its own host and
# nothing in the job log but the signal number: [save_core_dumps.sh] runs from
# the schedule's [after] block only when that host's own script failed, and a
# bench host's script is the control server, which exits 0 when the run shuts
# it down. So the core is written, never collected, and reaped with the job
# directory.
#
# Best effort throughout: this runs while a failure is already being reported
# and must not replace it with a failure of its own.
replay_crash_backtrace_for_label() {
  local wanted_label=$1
  local status=$2
  local record host label stdout_path stderr_path signal

  # Only a fatal signal leaves a core. A process that chose its own exit status
  # has already said what happened, in its own log.
  if [ "$status" -le 128 ] || [ "$status" -ge 192 ]; then
    return 0
  fi
  signal=$((status - 128))

  for record in ${CONTROL_LOG_RECORDS[@]+"${CONTROL_LOG_RECORDS[@]}"}; do
    IFS='|' read -r host label stdout_path stderr_path <<<"$record"
    [[ $label == "$wanted_label" ]] || continue
    echo "===== backtrace for $label on $host (signal $signal) ====="
    remote_run "$host" \
      "$(crash_backtrace_remote_script "$CRASH_BACKTRACE_MAX_LINES" \
        "${CRASH_CORE_SEARCH_DIRS[@]}")" ||
      echo "(could not reach $host for a backtrace)"
    echo "===== end of backtrace for $label on $host ====="
    return 0
  done

  echo "no control log record for $wanted_label" >&2
}
