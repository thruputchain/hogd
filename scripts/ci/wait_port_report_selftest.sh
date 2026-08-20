#!/usr/bin/env bash
# Self-test for wait_port_or_report: a wait that succeeds must behave like
# ci_wait_port, and a wait that times out must name the port, replay the logs of
# what was launched on that host, and still fail. Also covers
# await_mempool_run, which has to report the awaited mempool's own exit status
# and must not be confused by anything else the host was asked to start.
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

# Runs a body with the helper sourced and its dependencies stubbed.
# [ci_wait_port] fails for $UNREACHABLE_PORT and succeeds otherwise. A remote
# command that reads the mempool completion line answers with
# $MEMPOOL_COMPLETE_LINE when it is set, so a case can hand the helper the line
# a real mempool would have written.
run_case() {
  bash -c "
set -Eeuo pipefail
declare -a CONTROL_LOG_RECORDS=()
. '$HELPERS'
hog_ip() { printf '10.100.0.%s\n' \"\${1#hog}\"; }
ci_wait_port() {
  [ \"\$2\" != \"\${UNREACHABLE_PORT:-}\" ] || { echo \"Timed out: \$1:\$2\"; return 1; }
}
ci_describe_exit_status() { printf 'exit status %s' \"\$1\"; }
remote_run() {
  [ \"\${REMOTE_RUN_FAILS:-0}\" != 1 ] || { echo 'ssh: closed' >&2; return 255; }
  case \"\$2\" in
  *MEMPOOL_RUN_COMPLETE*)
    if [ -n \"\${MEMPOOL_COMPLETE_LINE+x}\" ]; then
      printf '%s\n' \"\$MEMPOOL_COMPLETE_LINE\"
      return 0
    fi
    ;;
  esac
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

# The timeout is overridable, for a milestone that is not a startup milestone
# and so is not bounded by the readiness budget.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_log_line node0_mempool "PORT mempool.db=8072" 42
') && status=0 || status=$?
expect_status 'log milestone honors an explicit timeout' 0 "$status"
expect_contains 'log milestone honors an explicit timeout' 'timeout 42s' "$out"

# A mempool whose run ended cleanly is awaited without a word about failure.
out=$(MEMPOOL_COMPLETE_LINE='MEMPOOL_RUN_COMPLETE status=0' run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
await_mempool_run node0_mempool 900
echo REACHED
') && status=0 || status=$?
expect_status 'mempool run completes' 0 "$status"
expect_contains 'mempool run completes' 'node0_mempool run completed' "$out"
expect_contains 'completed run continues' 'REACHED' "$out"
expect_lacks 'completed run replays nothing' 'last 80 lines' "$out"

# The status the mempool reported becomes the wait's own, so the suite fails
# with the mempool's status rather than a bare 1.
out=$(MEMPOOL_COMPLETE_LINE='MEMPOOL_RUN_COMPLETE status=134' run_case '
record hog5 node0_hog_exec_slot_a /tmp/exec.out /tmp/exec.err
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
await_mempool_run node0_mempool 900
echo NOT REACHED
') && status=0 || status=$?
expect_status 'failed mempool run reports its own status' 134 "$status"
expect_lacks 'failed mempool run stops the script' 'NOT REACHED' "$out"
expect_contains 'failed mempool run names the status' \
  'node0_mempool exit status 134' "$out"
expect_contains 'failed mempool run replays its log' \
  'REMOTE(hog7): tail -n 80 /tmp/mempool.out /tmp/mempool.err' "$out"
expect_lacks 'failed mempool run leaves other labels alone' '/tmp/exec.out' "$out"

# The completion line is read from the awaited mempool's own host and log, not
# from whatever the host started most recently.
out=$(MEMPOOL_COMPLETE_LINE='MEMPOOL_RUN_COMPLETE status=0' run_case '
record hog7 node0_mempool /tmp/mempool.out "/tmp/mempool err.log"
record hog7 node0_hog_db /tmp/db.out /tmp/db.err
await_mempool_run node0_mempool 900
') && status=0 || status=$?
expect_status 'await reads the mempool record' 0 "$status"
expect_lacks 'await ignores a later process on the same host' '/tmp/db.err' "$out"

# A run that never reports says so rather than waiting on something else.
out=$(REMOTE_RUN_FAILS=1 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
await_mempool_run node0_mempool 900
echo NOT REACHED
') && status=0 || status=$?
expect_status 'mempool never reports' 1 "$status"
expect_lacks 'mempool never reports stops the script' 'NOT REACHED' "$out"
expect_contains 'mempool never reports says so' \
  'node0_mempool never reported the end of its run' "$out"

# A completion line without a status is a failure to read it, not a status of 0.
out=$(MEMPOOL_COMPLETE_LINE='' run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
await_mempool_run node0_mempool 900
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unreadable exit status' 1 "$status"
expect_lacks 'unreadable exit status stops the script' 'NOT REACHED' "$out"
expect_contains 'unreadable exit status says so' \
  'could not read node0_mempool exit status from /tmp/mempool.err on hog7' "$out"

# Awaiting a label that was never launched says so rather than hanging.
out=$(MEMPOOL_COMPLETE_LINE='MEMPOOL_RUN_COMPLETE status=0' run_case '
await_mempool_run node1_mempool 900
echo NOT REACHED
') && status=0 || status=$?
expect_status 'await of an unknown label' 1 "$status"
expect_lacks 'await of an unknown label stops the script' 'NOT REACHED' "$out"
expect_contains 'await of an unknown label says so' \
  'no control log record for node1_mempool' "$out"

exit "$fail"
