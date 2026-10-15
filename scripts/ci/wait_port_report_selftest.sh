#!/usr/bin/env bash
# Self-test for wait_port_or_report: a wait that succeeds must behave like
# ci_wait_port, and a wait that times out must name the port, replay the logs of
# what was launched on that host, and still fail.
set -euo pipefail

HELPER_DIR="$(dirname "$(readlink -f "$0")")"
HELPERS="$HELPER_DIR/wait_port_report.sh"
# wait_port_report.sh replays panic reports through [ci_panic_report_program],
# which its callers get from the harness; the stubs below stand in for the rest.
HARNESS="$HELPER_DIR/bench_harness.sh"
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
. '$HARNESS'
. '$HELPERS'
hog_ip() { printf '10.100.0.%s\n' \"\${1#hog}\"; }
ci_wait_port() {
  [ \"\$2\" != \"\${UNREACHABLE_PORT:-}\" ] || { echo \"Timed out: \$1:\$2\"; return 1; }
}
remote_run() {
  [ \"\${REMOTE_RUN_FAILS:-0}\" != 1 ] || { echo 'ssh: closed' >&2; return 255; }
  # Naming the command is enough for the cases that only check what was asked
  # of the host. The panic replay has to read a real file to mean anything, so
  # those cases run the command instead.
  [ \"\${REMOTE_RUN_EXEC:-0}\" != 1 ] || { bash -c \"\$2\"; return; }
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

# An x23 panic prints its report and then dumps every live file descriptor and
# thread, so the tail of a crashed process's log is all dump and no reason.
# The replay names the assertion anyway.
crash_log="$(mktemp)"
trap 'rm -f "$crash_log"' EXIT
{
  echo 'I[1015 00:12:00 mempool.cc:12] mempool initialization complete.'
  echo 'hog-mempool:PANIC: x23assert failure: !*oldwi'
  echo '  in /tmp/hogjob-hogci/release/hogd/hog_mempool/hog-mempool'
  echo '  function docb'
  echo '  at  x23/x23/event_impl_epoll.cc:78'
  seq 1 500 | sed 's/^/  fd /;s/$/, mode 1, ready 4/'
} >"$crash_log"

out=$(REMOTE_RUN_EXEC=1 CRASH_LOG="$crash_log" run_case '
record hog11 node1_mempool /tmp/mempool.out "$CRASH_LOG"
replay_control_log_for_label node1_mempool
echo REACHED
') && status=0 || status=$?
expect_status 'panic replay' 0 "$status"
expect_contains 'panic replay has its own header' \
  'panic report from node1_mempool on hog11' "$out"
expect_contains 'panic replay names the assertion' \
  'x23assert failure: !*oldwi' "$out"
expect_contains 'panic replay names the source line' \
  'event_impl_epoll.cc:78' "$out"
expect_contains 'panic replay runs on' 'REACHED' "$out"

# A process that died without panicking must read exactly as it did before.
quiet_log="$(mktemp)"
printf 'I[1015 00:12:00 mempool.cc:12] mempool initialization complete.\n' \
  >"$quiet_log"
out=$(REMOTE_RUN_EXEC=1 QUIET_LOG="$quiet_log" run_case '
record hog11 node1_mempool /tmp/mempool.out "$QUIET_LOG"
replay_control_log_for_label node1_mempool
') && status=0 || status=$?
rm -f "$quiet_log"
expect_status 'replay without a panic' 0 "$status"
expect_lacks 'replay without a panic adds no header' 'panic report from' "$out"

exit "$fail"
