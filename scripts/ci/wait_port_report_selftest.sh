#!/usr/bin/env bash
# Self-test for wait_port_or_report: a wait that succeeds must behave like
# ci_wait_port, a wait that times out must name the port, replay the logs of
# what was launched on that host, and still fail, and a wait whose process has
# already exited must end there rather than run out its timeout.
set -euo pipefail

HELPERS="$(dirname "$(readlink -f "$0")")/wait_port_report.sh"
fail=0

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

# Stands in for [control_cli <host> <port> expect_running]. Answers per host so
# a case can have one host's launch dead and another's alive, and reproduces the
# real command's output: the answer is in what it printed, and its exit status
# of 1 is shared by every way of not knowing.
CONTROL_CLI="$work_dir/control_cli"
cat >"$CONTROL_CLI" <<'STUB'
#!/usr/bin/env bash
host=$1
if [[ " ${UNREACHABLE_HOSTS:-} " == *" $host "* ]]; then
  echo "I[0809 08:12:33 control_cli.cc:69] Error: connect($host:11111): Connection refused"
  exit 1
fi
if [[ " ${HANGING_HOSTS:-} " == *" $host "* ]]; then
  # [control_cli] retries a refused connection, so a server that has gone away
  # leaves the probe waiting rather than failing.
  sleep 30
  exit 0
fi
if [[ " ${EXITED_HOSTS:-} " == *" $host "* ]]; then
  echo "I[0809 08:12:33 control_cli.cc:44] Process is not running: exit code 134"
  exit 1
fi
echo "I[0809 08:12:33 control_cli.cc:39] Process is running"
exit 0
STUB
chmod +x "$CONTROL_CLI"
export CONTROL_CLI
export CI_CONTROL_PORT=11111

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
  [ \"\$2\" != \"\${UNREACHABLE_PORT:-}\" ] || {
    echo \"Timed out: \$1:\$2\"
    # The real wait fails only by running out its slice. Most cases do not care
    # how long that took and return at once; the ones timing the whole wait
    # need it to cost what it says it costs.
    [ \"\${SLOW_WAIT_PORT:-0}\" != 1 ] || sleep \"\${CI_WAIT_PORT_TIMEOUT_SEC:-0}\"
    return 1
  }
}
remote_run() {
  [ \"\${REMOTE_RUN_FAILS:-0}\" != 1 ] || { echo 'ssh: closed' >&2; return 255; }
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

## Giving up on a port whose process is already gone.
#
# The wait is run in slices and the host is asked between them, so the number of
# [ci_wait_port] calls -- one "Timed out" line each -- is how many slices ran.
# With the default 600s budget and 30s slices there are twenty; a wait that ends
# after one is a wait that ended on the answer rather than on its timeout.

expect_slices() {
  local label=$1 want=$2 haystack=$3
  local got
  got=$(grep -c 'Timed out:' <<<"$haystack" || true)
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: $got readiness slices, want $want"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

# The run that prompted this: both hog-exec shards on hog5 died a second after
# launch on "Address already in use". Ten minutes of waiting could not have
# produced a listener, and the port finally reported was not the one that failed
# to bind.
out=$(UNREACHABLE_PORT=8042 EXITED_HOSTS=hog5 CI_WAIT_PORT_TIMEOUT_SEC=600 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
record hog5 node0_hog_exec_slot_b /tmp/b.out /tmp/b.err
wait_port_or_report 10.100.0.5 8042
echo NOT REACHED
') && status=0 || status=$?
expect_status 'dead launch ends the script' 1 "$status"
expect_lacks 'dead launch stops the script' 'NOT REACHED' "$out"
expect_slices 'dead launch is not waited out' 1 "$out"
expect_contains 'dead launch names the port and host' \
  'readiness: 10.100.0.5:8042 is not going to come up; the last process launched on hog5 has already exited (exit code 134)' \
  "$out"
expect_contains 'dead launch replays the process that died' \
  'REMOTE(hog5): tail -n 80 /tmp/a.out /tmp/a.err' "$out"
expect_contains 'dead launch replays the other process on the host' \
  '/tmp/b.out' "$out"

# A host whose launch is still running gets the whole budget: a process can be
# slow to bind, and this must not turn a slow start into a failure.
out=$(UNREACHABLE_PORT=8042 CI_WAIT_PORT_TIMEOUT_SEC=60 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
wait_port_or_report 10.100.0.5 8042
echo NOT REACHED
') && status=0 || status=$?
expect_status 'live launch still times out' 1 "$status"
expect_slices 'live launch is waited out in full' 2 "$out"
expect_contains 'live launch reports the timeout' \
  'readiness: nothing is listening on 10.100.0.5:8042' "$out"
expect_lacks 'live launch is not called dead' 'has already exited' "$out"

# Not being able to ask is not an answer. A control server that cannot be
# reached, one that reports a version mismatch, and one asked before anything
# was launched all share [expect_running]'s exit status of 1, and reading any of
# them as "the process is gone" would abandon a port that was still coming up.
out=$(UNREACHABLE_PORT=8042 UNREACHABLE_HOSTS=hog5 CI_WAIT_PORT_TIMEOUT_SEC=60 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
wait_port_or_report 10.100.0.5 8042
') && status=0 || status=$?
expect_status 'unreachable control server still times out' 1 "$status"
expect_slices 'unreachable control server is waited out in full' 2 "$out"
expect_lacks 'unreachable control server is not read as an exit' \
  'has already exited' "$out"

# ...and a probe that never answers must not outlast the wait it was meant to
# cut short.
out=$(UNREACHABLE_PORT=8042 HANGING_HOSTS=hog5 CI_WAIT_PORT_TIMEOUT_SEC=60 \
  READINESS_PROC_CHECK_TIMEOUT_SEC=1 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
wait_port_or_report 10.100.0.5 8042
') && status=0 || status=$?
expect_status 'hanging probe still times out' 1 "$status"
expect_contains 'hanging probe reports the timeout' \
  'readiness: nothing is listening on 10.100.0.5:8042' "$out"

# A budget of 0 is [wait-port.sh]'s "wait forever", which has no last slice to
# trim and no timeout to reach. The check between slices is then the only thing
# that can end the wait.
out=$(UNREACHABLE_PORT=8042 EXITED_HOSTS=hog5 CI_WAIT_PORT_TIMEOUT_SEC=0 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
wait_port_or_report 10.100.0.5 8042
echo NOT REACHED
') && status=0 || status=$?
expect_status 'unbounded wait ends on a dead launch' 1 "$status"
expect_lacks 'unbounded wait stops the script' 'NOT REACHED' "$out"
expect_slices 'unbounded wait is not waited out' 1 "$out"

# ...and the budget covers the probes, not just the port waits. A probe that has
# to be abandoned is the signature of a control server that is wedged rather
# than merely down, so it is the case where every slice pays the probe timeout.
# Charged to nothing, those would carry the wait far past the timeout the caller
# configured -- with the production 600s budget and 30s slices, twenty probes at
# 20s each would end the wait around 1000s.
#
# The stub sleeps out its slices here so the whole wait can be timed: 8s of
# budget in 2s slices with a probe that costs 2s is four slices' worth of work,
# and it has to fit in the 8s rather than take the 16s it would if only the
# slices counted.
started=$SECONDS
out=$(UNREACHABLE_PORT=8042 HANGING_HOSTS=hog5 SLOW_WAIT_PORT=1 \
  CI_WAIT_PORT_TIMEOUT_SEC=8 READINESS_PROC_CHECK_INTERVAL_SEC=2 \
  READINESS_PROC_CHECK_TIMEOUT_SEC=2 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
wait_port_or_report 10.100.0.5 8042
') && status=0 || status=$?
elapsed=$((SECONDS - started))
expect_status 'wedged control server still times out' 1 "$status"
if ((elapsed > 12)); then
  echo "FAIL abandoned probes are charged to the budget: took ${elapsed}s, want <=12s"
  fail=1
else
  echo "ok   abandoned probes are charged to the budget (${elapsed}s)"
fi

# A port waited on before anything was launched has no host to ask, and the
# question must not be put to whichever host happens to be first.
out=$(UNREACHABLE_PORT=11222 EXITED_HOSTS=hog5 CI_WAIT_PORT_TIMEOUT_SEC=60 run_case '
record hog5 node0_hog_exec_slot_a /tmp/a.out /tmp/a.err
wait_port_or_report 10.100.0.4 11222
') && status=0 || status=$?
expect_status 'no records on this host' 1 "$status"
expect_slices 'no records on this host is waited out in full' 2 "$out"
expect_lacks 'no records on this host asks nobody' 'has already exited' "$out"

exit "$fail"
