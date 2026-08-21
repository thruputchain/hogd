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
  echo \"REMOTE(\$1): \$2\"
  [ \"\${REMOTE_RUN_TIMES_OUT:-0}\" != 1 ] || return 124
  [ -z \"\${REMOTE_EXIT_STATUS:-}\" ] ||
    echo \"CONTROL_EXIT_STATUS=\$REMOTE_EXIT_STATUS\"
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

# An exit wait reports the status the process recorded, not the status of
# whatever the control server happens to have launched most recently.
out=$(REMOTE_EXIT_STATUS=0 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_exit_status node0_mempool /work/node0/mempool.exit
echo REACHED
') && status=0 || status=$?
expect_status 'recorded clean exit' 0 "$status"
expect_contains 'clean exit names the status' \
  'node0_mempool on hog7 exited with status 0' "$out"
expect_contains 'clean exit runs on' 'REACHED' "$out"

out=$(REMOTE_EXIT_STATUS=2 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_exit_status node0_mempool /work/node0/mempool.exit
echo NOT REACHED
') && status=0 || status=$?
expect_status 'recorded failure is returned' 2 "$status"
expect_contains 'recorded failure names the status' \
  'node0_mempool on hog7 exited with status 2' "$out"
expect_lacks 'recorded failure stops the script' 'NOT REACHED' "$out"

# A marker that never appears is a wait that did not happen, not a clean exit.
out=$(REMOTE_RUN_TIMES_OUT=1 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_exit_status node0_mempool /work/node0/mempool.exit
echo NOT REACHED
') && status=0 || status=$?
expect_status 'exit wait times out' 124 "$status"
expect_contains 'exit wait timeout names the process' \
  'Timed out after 900s waiting for node0_mempool on hog7 to exit' "$out"
expect_lacks 'exit wait timeout stops the script' 'NOT REACHED' "$out"

out=$(REMOTE_RUN_FAILS=1 run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_exit_status node0_mempool /work/node0/mempool.exit
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unreachable host during exit wait' 255 "$status"
expect_contains 'unreachable host during exit wait says so' \
  'could not wait for node0_mempool on hog7 to exit (status 255)' "$out"
expect_lacks 'unreachable host during exit wait stops the script' \
  'NOT REACHED' "$out"

# A marker holding something other than a status fails the wait and shows what
# was read instead, and the marker path reaches the remote shell quoted.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
wait_control_exit_status node0_mempool "/work/a b/mempool.exit"
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unusable recorded status' 1 "$status"
expect_contains 'unusable recorded status says so' \
  'recorded no usable exit status in /work/a b/mempool.exit' "$out"
expect_contains 'exit wait quotes the marker path' \
  '/work/a\\\ b/mempool.exit' "$out"
expect_lacks 'unusable recorded status stops the script' 'NOT REACHED' "$out"

out=$(REMOTE_EXIT_STATUS=0 run_case '
wait_control_exit_status node1_mempool /work/node1/mempool.exit
echo NOT REACHED
') && status=0 || status=$?
expect_status 'exit wait for an unknown label' 1 "$status"
expect_contains 'exit wait for an unknown label says so' \
  'no control log record for node1_mempool' "$out"
expect_lacks 'exit wait for an unknown label stops the script' \
  'NOT REACHED' "$out"

# A log copied for publication is capped, and the remote program it runs to do
# that quotes the path and keeps the tail rather than the head.
out=$(run_case '
copy_remote_log hog7 "/tmp/mempool err.log" /dev/stdout
') && status=0 || status=$?
expect_status 'capped copy' 0 "$status"
expect_contains 'capped copy quotes the remote path' \
  'wc -c </tmp/mempool\ err.log' "$out"
expect_contains 'capped copy keeps the tail' \
  'tail -c 8388608 /tmp/mempool\ err.log' "$out"

out=$(PUBLISHED_LOG_MAX_BYTES=64 run_case '
copy_remote_log hog7 /tmp/mempool.log /dev/stdout
') && status=0 || status=$?
expect_status 'capped copy honours the cap override' 0 "$status"
expect_contains 'capped copy honours the cap override' \
  'tail -c 64 /tmp/mempool.log' "$out"

# An unreadable log must stay as loud as a failed copy was.
out=$(REMOTE_RUN_FAILS=1 run_case '
copy_remote_log hog7 /tmp/mempool.log /dev/null
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unreadable log during a capped copy' 255 "$status"
expect_lacks 'unreadable log stops the script' 'NOT REACHED' "$out"

# The remote side of the copy, run for real: a log under the cap is copied
# whole and unannotated, and one over it keeps exactly the last cap bytes and
# says how much it dropped.
work=$(mktemp -d)
printf 'head\nmiddle\ntail\n' >"$work/small.log"
out=$(bash -c "$(
  . "$HELPERS"
  capped_log_program "$(printf '%q' "$work/small.log")" 1024
)") && status=0 || status=$?
expect_status 'remote program, log under the cap' 0 "$status"
expect_contains 'log under the cap is copied whole' 'head' "$out"
expect_lacks 'log under the cap is not annotated' 'truncated' "$out"

# 300 bytes of 'a' then a 10-byte marker; a 32-byte cap must land inside the
# marker and drop 278 bytes.
{
  printf 'a%.0s' $(seq 300)
  printf 'ENDMARKER1'
} >"$work/big.log"
out=$(bash -c "$(
  . "$HELPERS"
  capped_log_program "$(printf '%q' "$work/big.log")" 32
)") && status=0 || status=$?
expect_status 'remote program, log over the cap' 0 "$status"
expect_contains 'log over the cap names what it dropped' \
  '[truncated: 278 of 310 bytes dropped; the last 32 bytes follow]' "$out"
expect_contains 'log over the cap keeps the tail' 'ENDMARKER1' "$out"
bytes=$(printf '%s' "$out" | tail -c 32 | wc -c | tr -d ' ')
expect_status 'log over the cap keeps exactly the cap' 32 "$bytes"

out=$(bash -c "$(
  . "$HELPERS"
  capped_log_program "$(printf '%q' "$work/missing.log")" 32
)" 2>/dev/null) && status=0 || status=$?
if [[ $status -eq 0 ]]; then
  echo 'FAIL remote program, missing log: exited 0, want nonzero'
  fail=1
else
  echo 'ok   remote program, missing log'
fi
rm -rf "$work"

exit "$fail"
