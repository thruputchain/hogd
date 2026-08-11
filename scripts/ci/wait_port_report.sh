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

# How many candidate cores to identify before giving up on finding the one that
# belongs to the crashed process. Each costs one gdb read of the core's note
# section, so this bounds the work on a host littered with cores.
CRASH_MAX_CANDIDATE_CORES=${CRASH_MAX_CANDIDATE_CORES:-8}

# The shell fragment that turns a core file into a backtrace, run on the host
# that produced it. The search directories arrive as positional parameters and
# everything else as assignments, so the body itself stays single-quoted and
# nothing in it is expanded here on the way through ssh.
#
# Usage: crash_backtrace_remote_script <binary> <max lines> <max cores> \
#            <search dir>...
crash_backtrace_remote_script() {
  local binary=$1
  local max_lines=$2
  local max_cores=$3
  local quoted_binary quoted_dirs
  shift 3

  printf -v quoted_binary '%q' "$binary"
  printf -v quoted_dirs '%q ' "$@"
  printf 'CRASH_BINARY=%s\nCRASH_MAX_LINES=%s\nCRASH_MAX_CORES=%s\nset -- %s\n%s\n' \
    "$quoted_binary" "$max_lines" "$max_cores" "${quoted_dirs% }" '
# Collect every candidate across every directory, newest first. Not
# first-directory-wins: a stale core left in the job directory by an earlier
# run must not hide the fresh one the kernel just wrote somewhere else.
# "$HOME" comes last because only this shell knows what it is.
patterns=
for dir in "$@" "$HOME"; do
  [ -d "$dir" ] || continue
  # The kernel names the core after kernel.core_pattern, which is a bare "core"
  # on some hosts and "core.<pid>" on others.
  patterns="$patterns $dir/core $dir/core.* $dir/*.core"
done
# Unquoted on purpose: this is where the globs above are expanded. Patterns
# that match nothing stay literal and ls rejects them into /dev/null.
candidates=$(ls -1td $patterns 2>/dev/null | head -n "$CRASH_MAX_CORES")

if [ -z "$candidates" ]; then
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

if ! command -v gdb >/dev/null 2>&1; then
  echo "(gdb is not installed on this host; cores left in place:"
  echo "$candidates" | sed "s/^/  /"
  echo ")"
  exit 0
fi

# The core records the path the process was launched from, which is how
# [save_core_dumps.sh] pairs a core with its binary. Here it also says which
# core is ours: this benchmark puts a hog-db on the same host as each mempool,
# so a cascading crash leaves more than one, and the newest is not necessarily
# the one being reported on.
core_exe() {
  found=$(gdb -batch -q -c "$1" -ex "info auxv" 2>/dev/null |
    sed -n "s/.*AT_EXECFN[^\"]*\"\(.*\)\"$/\1/p")
  # AT_EXECFN is the path as the process was invoked, so it is relative when
  # the process was started that way -- relative to the directory it ran in,
  # which is the directory the kernel wrote the core into.
  case $found in
  /* | "") ;;
  *) found=${1%/*}/${found#./} ;;
  esac
  printf "%s" "$found"
}

want=${CRASH_BINARY##*/}
core= exe= newest= newest_exe=
for candidate in $candidates; do
  candidate_exe=$(core_exe "$candidate")
  if [ -z "$newest" ]; then
    newest=$candidate
    newest_exe=$candidate_exe
  fi
  if [ -n "$want" ] && [ "${candidate_exe##*/}" = "$want" ]; then
    core=$candidate
    exe=$candidate_exe
    break
  fi
done

if [ -n "$core" ]; then
  echo "core file: $core (from $exe)"
else
  # Say so rather than passing an unrelated crash off as this one.
  core=$newest
  exe=$newest_exe
  if [ -n "$want" ]; then
    echo "no core among the newest $CRASH_MAX_CORES on this host came from" \
      "$want; the newest core is shown instead and may be an unrelated crash"
  fi
  echo "core file: $core (from ${exe:-an unidentified binary})"
fi

# Without the binary gdb still unwinds, but the frames have no symbols.
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
#
# Usage: replay_crash_backtrace_for_label <label> <exit status> [binary]
#
# The binary is the one the label was launched as. Pass it wherever it is
# known: bench hosts run more than one service each, so on a cascading crash it
# is what tells this process's core apart from its neighbour's.
replay_crash_backtrace_for_label() {
  local wanted_label=$1
  local status=$2
  local binary=${3:-}
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
      "$(crash_backtrace_remote_script "$binary" "$CRASH_BACKTRACE_MAX_LINES" \
        "$CRASH_MAX_CANDIDATE_CORES" "${CRASH_CORE_SEARCH_DIRS[@]}")" ||
      echo "(could not reach $host for a backtrace)"
    echo "===== end of backtrace for $label on $host ====="
    return 0
  done

  echo "no control log record for $wanted_label" >&2
}
