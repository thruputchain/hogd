#!/usr/bin/env bash
# Frees a bench control port so this run's [control_server] can bind it.
#
# Usage: reap_control_server.sh <port>
#
# A control server outlives the run that started it often enough that every
# schedule reaps one before starting its own. Reaping by name alone is not
# enough: it leaves the run to find out whether the port is actually free by
# binding it, and a bind that loses throws out of [stcplisten] and aborts the
# process with
#
#     terminate called after throwing an instance of 'std::system_error'
#       what():  bind in6addr_any: Address already in use
#
# which is a core dump and an exit 134 for the whole job, with nothing in the
# log naming what held the port. [killall control_server] returning "no
# process found" while the port stays bound is exactly the case that produced
# hogd issue #3940, and by-name reaping cannot see it.
#
# So reap by what has to be true -- the port has to be free -- and name the
# holder when it cannot be made free, instead of leaving the bind to abort.
set -Eeuo pipefail

if [ $# -ne 1 ]; then
  echo "Usage: $0 <port>" >&2
  exit 1
fi

PORT=$1

# How long the holders get to exit on SIGTERM before the reap escalates to
# SIGKILL, and how long SIGKILL then gets to take effect.
REAP_TERM_TIMEOUT_SEC=${REAP_TERM_TIMEOUT_SEC:-20}
REAP_KILL_TIMEOUT_SEC=${REAP_KILL_TIMEOUT_SEC:-10}

command -v killall >/dev/null || {
  echo 'killall not found (install psmisc)' >&2
  exit 1
}

# The tool that says who holds a port. Overridable so the self-test can point
# it at a stub, and at a name that is not installed.
REAP_SS=${REAP_SS:-ss}

# The flags [query_listeners] asks for, and the ones it falls back to. [-e]
# adds the socket's owning uid and inode, which is the only ownership this
# script gets for a holder it cannot attribute to a pid -- and the holder it
# cannot attribute is exactly the one it cannot reap, so it is the one whose
# owner the failure has to name.
REAP_SS_FLAGS=-Hltnpe
REAP_SS_FLAGS_PLAIN=-Hltnp

# Reaping by port needs that tool. A host without it must fall back to the
# by-name reap this script replaced rather than fail: it is the weaker reap,
# but failing here would take down a host the old reap served, which is a
# worse outcome than the bind race this script closes.
if ! command -v "$REAP_SS" >/dev/null; then
  echo "warning: $REAP_SS not found (install iproute2); reaping by name only" >&2
  killall -TERM control_server 2>/dev/null && sleep 3 || true
  killall -KILL control_server 2>/dev/null || true
  exit 0
fi

# The listening sockets on $PORT from the most recent query, one raw [ss] line
# each. Empty means the port is free, which is the only thing this script has
# to establish.
REAP_LISTENERS=""

# Refreshes [REAP_LISTENERS]. Fails if the query itself failed -- an
# unsupported filter, a netlink error -- rather than reporting no output as an
# empty port: a query that could not run has not shown the port to be free,
# and passing on that reading is how this script would hand the next bind the
# very abort it exists to prevent. [ss] diagnostics are left on stderr, where
# the job log wants them; only stdout is the answer.
query_listeners() {
  local status=0

  REAP_LISTENERS=$("$REAP_SS" "$REAP_SS_FLAGS" "sport = :$PORT" 2>/dev/null) ||
    status=$?
  if [ "$status" -ne 0 ]; then
    # [-e] is not worth failing the reap over: it only enriches the report of
    # a holder that cannot be reaped anyway, and an [ss] too old to take it
    # still answers the question the reap turns on. Retry without it -- but
    # decided per query, never latched: a netlink error or a bad filter fails
    # both forms, so it is reported below instead of being mistaken for an
    # [ss] without [-e] and quietly costing every later query the ownership
    # this script exists to report. The first attempt's stderr is dropped
    # because it is noise when a retry follows; the retry's is the diagnostic.
    status=0
    REAP_LISTENERS=$("$REAP_SS" "$REAP_SS_FLAGS_PLAIN" "sport = :$PORT") ||
      status=$?
  fi
  if [ "$status" -ne 0 ]; then
    REAP_LISTENERS=""
    echo "$REAP_SS exited $status querying port $PORT; cannot establish" \
      "that the port is free" >&2
    return 1
  fi
  return 0
}

# Polls until the port is free, for up to $1 seconds. Fails if the port stays
# held, and fails immediately if the query stops working, for the same reason.
wait_port_free() {
  local timeout=$1
  local start=$SECONDS

  while true; do
    query_listeners || return 1
    if [ -z "$REAP_LISTENERS" ]; then
      return 0
    fi
    if [ $((SECONDS - start)) -ge "$timeout" ]; then
      return 1
    fi
    sleep 0.2
  done
}

# Signals the holders named by the most recent query. [ss] reports process
# details only for sockets the calling user owns, so a port held by another
# user's process yields a listener line with no pid -- which still counts as
# held, and which the failure below reports rather than silently passing.
signal_listeners() {
  local signal=$1
  local pid
  local -a pids=()

  # Read the holders from one snapshot: signalling walks the list, and a pid
  # that exits partway through must not shorten it.
  mapfile -t pids < <(
    printf '%s\n' "$REAP_LISTENERS" | grep -o 'pid=[0-9]*' | cut -d= -f2 |
      sort -u
  )
  if [ "${#pids[@]}" -eq 0 ]; then
    return 0
  fi

  for pid in "${pids[@]}"; do
    echo "sending SIG$signal to pid $pid holding port $PORT"
    kill "-$signal" "$pid" 2>/dev/null || true
  done
}

# The account name for uid [$1], or the bare uid if it cannot be resolved.
uid_name() {
  local uid=$1
  local name

  name=$(getent passwd "$uid" 2>/dev/null | cut -d: -f1) || name=""
  if [ -n "$name" ]; then
    echo "$uid ($name)"
  else
    echo "$uid"
  fi
}

# Says who holds each socket in [REAP_LISTENERS], one line per listener.
#
# The raw [ss] line is not an answer on its own: a holder this account cannot
# attribute prints as a bare address with an empty process column, which is
# what the reap that could not free port 11111 in hogd issue #3947 left in the
# log -- a failure naming nothing to act on. [ss] withholds the process for a
# socket owned by another account, but [-e] still reports that account's uid,
# and a listener with an inode and no uid at all is root's, since [ss] prints
# [uid:] only when it is nonzero. So name the owner from whatever the line
# carries, and say which account looked, because "another account owns it" is
# the difference between reaping harder and waking someone with a root shell.
describe_holders() {
  local line pid uid ino
  local whoami
  whoami=$(id -un 2>/dev/null || echo "uid $(id -u)")

  # Each field is absent on some listener this has to describe -- that is the
  # case being described -- and [grep] reports absence as failure, which under
  # this script's errexit would abort the report instead of writing it.
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    pid=$(printf '%s\n' "$line" | grep -o 'pid=[0-9]*' | cut -d= -f2 |
      sort -u | paste -sd, - || true)
    if [ -n "$pid" ]; then
      echo "  held by pid $pid, which outlived SIGKILL"
      continue
    fi
    ino=$(printf '%s\n' "$line" | grep -o 'ino:[0-9]*' | cut -d: -f2 |
      head -1 || true)
    if [ -z "$ino" ]; then
      echo "  holder not attributable: $REAP_SS reported no process and no" \
        "socket details for this listener"
      continue
    fi
    uid=$(printf '%s\n' "$line" | grep -o 'uid:[0-9]*' | cut -d: -f2 |
      head -1 || true)
    # [ss] omits [uid:] for a socket owned by root, so an inode with no uid is
    # uid 0 rather than an unknown owner.
    echo "  held by socket inode $ino owned by uid $(uid_name "${uid:-0}");" \
      "this reap runs as $whoami and cannot signal it"
  done <<<"$REAP_LISTENERS"
}

# A query that cannot answer is a failed reap, not a free port.
query_listeners || exit 1

if [ -z "$REAP_LISTENERS" ]; then
  echo "port $PORT is free"
  exit 0
fi

echo "port $PORT is held; reaping:"
printf '%s\n' "$REAP_LISTENERS"

# Ask the previous run's server to come down the way its own shutdown would,
# so it still gets to reap the processes it launched. [killall] covers a
# server whose listening socket this host cannot attribute to a pid.
killall -TERM control_server 2>/dev/null || true
signal_listeners TERM
if wait_port_free "$REAP_TERM_TIMEOUT_SEC"; then
  echo "port $PORT is free"
  exit 0
fi

echo "port $PORT still held after ${REAP_TERM_TIMEOUT_SEC}s; escalating" >&2
killall -KILL control_server 2>/dev/null || true
signal_listeners KILL
if wait_port_free "$REAP_KILL_TIMEOUT_SEC"; then
  echo "port $PORT is free"
  exit 0
fi

# Nothing this script can do frees the port -- most often a holder belonging
# to another user, which it may not signal. Say so here, where the holder is
# still visible, rather than letting the bind abort with only an errno. The
# snapshot is whatever the last query returned, and is empty only when that
# query failed, which reported itself above.
echo "could not free port $PORT" >&2
if [ -n "$REAP_LISTENERS" ]; then
  echo "still held by:" >&2
  printf '%s\n' "$REAP_LISTENERS" >&2
  describe_holders >&2
fi
exit 1
