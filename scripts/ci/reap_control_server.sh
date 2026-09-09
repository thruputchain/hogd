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
command -v ss >/dev/null || {
  echo 'ss not found (install iproute2)' >&2
  exit 1
}

# The listening sockets on $PORT, one raw [ss] line each. Empty output means
# the port is free, which is the only thing this script has to establish.
listeners() {
  ss -Hltnp "sport = :$PORT" 2>/dev/null || true
}

# The pids holding those sockets. [ss] reports process details only for
# sockets the calling user owns, so a port held by another user's process
# yields a listener line with no pid -- which [port_free] still sees as held,
# and which the failure below reports rather than silently passing.
listener_pids() {
  listeners | grep -o 'pid=[0-9]*' | cut -d= -f2 | sort -u
}

port_free() {
  [ -z "$(listeners)" ]
}

# Polls until the port is free, for up to $1 seconds.
wait_port_free() {
  local timeout=$1
  local start=$SECONDS

  while ! port_free; do
    if [ $((SECONDS - start)) -ge "$timeout" ]; then
      return 1
    fi
    sleep 0.2
  done
  return 0
}

signal_listeners() {
  local signal=$1
  local pid
  local -a pids=()

  # Read the holders once: signalling walks the list, and a pid that exits
  # partway through must not shorten it.
  mapfile -t pids < <(listener_pids)
  if [ "${#pids[@]}" -eq 0 ]; then
    return 0
  fi

  for pid in "${pids[@]}"; do
    echo "sending SIG$signal to pid $pid holding port $PORT"
    kill "-$signal" "$pid" 2>/dev/null || true
  done
}

if port_free; then
  echo "port $PORT is free"
  exit 0
fi

echo "port $PORT is held; reaping:"
listeners

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
# still visible, rather than letting the bind abort with only an errno.
echo "could not free port $PORT; still held by:" >&2
listeners >&2
exit 1
