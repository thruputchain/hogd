#!/usr/bin/env bash
# Self-test for wait_port_or_report: a wait that succeeds must behave like
# ci_wait_port, and a wait that times out must name the port, replay the logs of
# what was launched on that host, and still fail.
set -euo pipefail

HELPERS="$(dirname "$(readlink -f "$0")")/wait_port_report.sh"
fail=0

expect_status() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: exit status $got, want $want"
    fail=1
  else
    echo "ok   $label status"
  fi
}

expect_contains() {
  local label=$1 needle=$2 haystack=$3
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL $label: output missing [$needle]"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

expect_equals() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: got [$got], want [$want]"
    fail=1
  else
    echo "ok   $label"
  fi
}

expect_lacks() {
  local label=$1 needle=$2 haystack=$3
  if [[ "$haystack" == *"$needle"* ]]; then
    echo "FAIL $label: output should not contain [$needle]"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

# Runs a body with the helper sourced and its three dependencies stubbed.
# [ci_wait_port] fails for $UNREACHABLE_PORT and succeeds otherwise.
run_case() {
  bash -c "
set -Eeuo pipefail
declare -a CONTROL_LOG_RECORDS=()
. '$HELPERS'
hog_ip() { printf '10.100.0.%s\n' \"\${1#hog}\"; }
ci_wait_port() {
  [ \"\$2\" != \"\${UNREACHABLE_PORT:-}\" ] || { echo \"Timed out: \$1:\$2\"; return 1; }
}
remote_run() {
  [ \"\${REMOTE_RUN_FAILS:-0}\" != 1 ] || { echo 'ssh: closed' >&2; return 255; }
  if [ -n \"\${REMOTE_RUN_STDOUT+set}\" ]; then
    printf '%s\n' \"\$REMOTE_RUN_STDOUT\"
    return 0
  fi
  echo \"REMOTE(\$1): \$2\"
}
record() { CONTROL_LOG_RECORDS+=(\"\$1|\$2|\$3|\$4\"); }
$1
" 2>&1
}

# A port that comes up is not reported, and the wait succeeds.
out=$(run_case '
record hog5 node0_hog_exec_slot_a /tmp/out.log /tmp/err.log
wait_port_or_report 10.100.0.5 8040
echo REACHED
') && status=0 || status=$?
expect_status 'port comes up' 0 "$status"
expect_contains 'successful wait runs on' 'REACHED' "$out"
expect_lacks 'successful wait is silent' 'readiness:' "$out"

# A timed-out wait names the port, replays both logs of every process on that
# host, leaves other hosts alone, and still ends the script.
out=$(UNREACHABLE_PORT=8040 run_case '
record hog5 node0_hog_exec_slot_a "/tmp/a b/out.log" /tmp/err.log
record hog5 node0_hog_exec_slot_b /tmp/b.out /tmp/b.err
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_port_or_report 10.100.0.5 8040
echo NOT REACHED
') && status=0 || status=$?
expect_status 'timed-out wait ends the script' 1 "$status"
expect_lacks 'timed-out wait stops the script' 'NOT REACHED' "$out"
expect_contains 'timeout names the port' \
  'readiness: nothing is listening on 10.100.0.5:8040' "$out"
expect_contains 'names the process' 'node0_hog_exec_slot_a on hog5' "$out"
expect_contains 'replays stdout and stderr, quoted' \
  'REMOTE(hog5): tail -n 80 /tmp/a\ b/out.log /tmp/err.log' "$out"
expect_contains 'replays the other process on the host' '/tmp/b.out' "$out"
expect_lacks 'leaves other hosts alone' 'mempool.out' "$out"

# The replay is best effort: an unreachable host must still leave the readiness
# timeout as the reason the script ended.
out=$(UNREACHABLE_PORT=8040 REMOTE_RUN_FAILS=1 run_case '
record hog5 node0_hog_exec_slot_a /tmp/out.log /tmp/err.log
wait_port_or_report 10.100.0.5 8040
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unreachable host during replay' 1 "$status"
expect_contains 'unreachable host still reports' 'nothing is listening' "$out"
expect_lacks 'unreachable host does not continue' 'NOT REACHED' "$out"

# A port waited on before anything was launched has no records to replay, which
# is not itself fatal.
out=$(UNREACHABLE_PORT=11222 run_case '
wait_port_or_report 10.100.0.4 11222
echo NOT REACHED
') && status=0 || status=$?
expect_status 'no records yet' 1 "$status"
expect_contains 'no records still reports' \
  'readiness: nothing is listening on 10.100.0.4:11222' "$out"
expect_lacks 'no records does not continue' 'NOT REACHED' "$out"

# A process that came up and died later is replayed by label, since no
# readiness wait is left to report it.
out=$(run_case '
record hog5 node0_hog_exec_slot_a /tmp/exec.out /tmp/exec.err
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
replay_control_log_for_label node0_mempool
echo REACHED
') && status=0 || status=$?
expect_status 'replay by label' 0 "$status"
expect_contains 'replay by label names the process' 'node0_mempool on hog7' "$out"
expect_contains 'replay by label replays both streams' \
  'REMOTE(hog7): tail -n 80 /tmp/mempool.out /tmp/mempool.err' "$out"
expect_lacks 'replay by label leaves other labels alone' '/tmp/exec.out' "$out"

# An unknown label says so rather than failing the run that is already failing.
out=$(run_case '
replay_control_log_for_label node1_mempool
echo REACHED
') && status=0 || status=$?
expect_status 'replay of an unknown label' 0 "$status"
expect_contains 'replay of an unknown label says so' \
  'no control log record for node1_mempool' "$out"
expect_contains 'replay of an unknown label runs on' 'REACHED' "$out"

# Log milestones provide non-invasive RPC readiness and preserve diagnostics.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out "/tmp/mempool err.log"
wait_control_log_line node0_mempool "PORT mempool.executor=8071"
echo REACHED
') && status=0 || status=$?
expect_status 'log milestone appears' 0 "$status"
expect_contains 'log milestone runs on' 'REACHED' "$out"
expect_contains 'log milestone quotes the remote path' '/tmp/mempool\\\ err.log' "$out"

out=$(REMOTE_RUN_FAILS=1 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_line node0_mempool "PORT mempool.executor=8071"
echo NOT REACHED
') && status=0 || status=$?
expect_status 'log milestone timeout ends the script' 1 "$status"
expect_lacks 'log milestone timeout stops the script' 'NOT REACHED' "$out"
expect_contains 'log milestone timeout names the process' \
  'waiting for node0_mempool to report: PORT mempool.executor=8071' "$out"
expect_contains 'log milestone timeout replays logs' \
  'node0_mempool on hog7' "$out"

# A process reports its own outcome by writing a marker as it exits, and the
# wait yields whatever follows the marker -- and only that, so a caller can
# capture it.
out=$(REMOTE_RUN_STDOUT='I[0818 12:57:38] MEMPOOL_RUN_COMPLETE status=2' run_case '
record hog5 node0_hog_exec_slot_a /tmp/exec.out /tmp/exec.err
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 2>/dev/null
') && status=0 || status=$?
expect_status 'marker wait' 0 "$status"
expect_equals 'marker wait yields only what follows the marker' '2' "$out"

# A zero status is a result like any other, not an absent one.
out=$(REMOTE_RUN_STDOUT='MEMPOOL_RUN_COMPLETE status=0' run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 2>/dev/null
') && status=0 || status=$?
expect_status 'zero marker value' 0 "$status"
expect_equals 'zero marker value is yielded' '0' "$out"

# The wait reads the end of the log rather than all of it, and looks at a pace
# that does not compete with the benchmark writing it.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out "/tmp/mempool err.log"
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 660 2>&1
') && status=0 || status=$?
expect_contains 'marker wait reads only the tail' 'tail\ -n\ 20' "$out"
expect_contains 'marker wait polls slowly' 'sleep\ 2' "$out"
expect_contains 'marker wait quotes the remote path' '/tmp/mempool\\\ err.log' "$out"
expect_contains 'marker wait honours an explicit deadline' 'timeout 660s' "$out"

# A deadline of 0 is [timeout]'s "no deadline", for a run that only ends when
# it is stopped by hand.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 0 2>&1
') && status=0 || status=$?
expect_contains 'zero deadline waits without one' 'timeout 0s' "$out"

# A wait with no deadline that ends anyway -- its host went away -- must not
# blame a deadline that was never set.
out=$(REMOTE_RUN_FAILS=1 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 0 2>&1
echo NOT REACHED
') && status=0 || status=$?
expect_status 'undeadlined wait that ends' 1 "$status"
expect_contains 'undeadlined wait says it stopped' \
  'Stopped waiting for node0_mempool to report' "$out"
expect_lacks 'undeadlined wait claims no timeout' 'Timed out after 0s' "$out"

# A wait that times out names the process, replays its log, and still ends the
# script rather than yielding a status the process never reported.
out=$(REMOTE_RUN_FAILS=1 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 2>&1
echo NOT REACHED
') && status=0 || status=$?
expect_status 'marker wait timeout ends the script' 1 "$status"
expect_contains 'marker wait timeout names the process' \
  'waiting for node0_mempool to report: MEMPOOL_RUN_COMPLETE status=' "$out"
expect_contains 'marker wait timeout replays logs' 'node0_mempool on hog7' "$out"
expect_lacks 'marker wait timeout stops the script' 'NOT REACHED' "$out"

# A line that came back without the marker is not a status of its own.
out=$(REMOTE_RUN_STDOUT='' run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node0_mempool "MEMPOOL_RUN_COMPLETE status=" 2>&1
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unreadable marker line fails' 1 "$status"
expect_contains 'unreadable marker line says so' \
  'node0_mempool reported an unreadable [MEMPOOL_RUN_COMPLETE status=] line' "$out"
expect_lacks 'unreadable marker line stops the script' 'NOT REACHED' "$out"

# An unknown label says so rather than reporting someone else's marker.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_marker node1_mempool "MEMPOOL_RUN_COMPLETE status=" 2>&1
echo NOT REACHED
') && status=0 || status=$?
expect_status 'marker wait of an unknown label' 1 "$status"
expect_contains 'marker wait of an unknown label says so' \
  'no control log record for node1_mempool' "$out"
expect_lacks 'marker wait of an unknown label stops the script' \
  'NOT REACHED' "$out"

exit "$fail"
