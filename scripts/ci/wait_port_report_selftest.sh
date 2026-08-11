#!/usr/bin/env bash
# Self-test for wait_port_or_report: a wait that succeeds must behave like
# ci_wait_port, and a wait that times out must name the port, replay the logs of
# what was launched on that host, and still fail.
#
# Also covers replay_crash_backtrace_for_label, including an end-to-end pass
# over a core this script produces itself.
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

# A process that chose its own exit status has already said what happened, so
# no core is looked for.
out=$(run_case '
record hog7 node0_mempool /tmp/mempool.out /tmp/mempool.err
replay_crash_backtrace_for_label node0_mempool 1
echo REACHED
') && status=0 || status=$?
expect_status 'non-signal exit' 0 "$status"
expect_lacks 'non-signal exit looks for no core' 'backtrace for' "$out"
expect_contains 'non-signal exit runs on' 'REACHED' "$out"

# A fatal signal names the process, its host and the signal, and asks that host
# for the backtrace.
out=$(run_case '
record hog5 node1_hog_exec_slot_a /tmp/exec.out /tmp/exec.err
record hog11 node1_mempool /tmp/mempool.out /tmp/mempool.err
replay_crash_backtrace_for_label node1_mempool 139
echo REACHED
') && status=0 || status=$?
expect_status 'signal death' 0 "$status"
expect_contains 'signal death names process, host and signal' \
  'backtrace for node1_mempool on hog11 (signal 11)' "$out"
expect_contains 'signal death asks the right host' 'REMOTE(hog11):' "$out"
expect_contains 'signal death looks for a core' 'core.*' "$out"
expect_lacks 'signal death leaves other hosts alone' 'hog5' "$out"
expect_contains 'signal death runs on' 'REACHED' "$out"

# The backtrace is best effort: a host that has gone away must leave the crash
# itself as the reason the script ended.
out=$(REMOTE_RUN_FAILS=1 run_case '
record hog11 node1_mempool /tmp/mempool.out /tmp/mempool.err
replay_crash_backtrace_for_label node1_mempool 139
echo REACHED
') && status=0 || status=$?
expect_status 'unreachable host during backtrace' 0 "$status"
expect_contains 'unreachable host says so' \
  '(could not reach hog11 for a backtrace)' "$out"
expect_contains 'unreachable host runs on' 'REACHED' "$out"

# A crash reported for a label that was never launched says so rather than
# failing the run that is already failing.
out=$(run_case '
replay_crash_backtrace_for_label node1_mempool 139
echo REACHED
') && status=0 || status=$?
expect_status 'crash for an unknown label' 0 "$status"
expect_contains 'crash for an unknown label says so' \
  'no control log record for node1_mempool' "$out"
expect_contains 'crash for an unknown label runs on' 'REACHED' "$out"

# End to end over a real core: the fragment that runs on the crashed process's
# host must find the core, pair it with its binary, and print the crashing
# frame. Skipped where the toolchain or the kernel's core handling is not there
# to test against, which must not be reported as a failure.
crash_dir=$(mktemp -d)
trap 'rm -rf "$crash_dir"' EXIT
if ! command -v gdb >/dev/null 2>&1 || ! command -v cc >/dev/null 2>&1; then
  echo "skip end-to-end backtrace (need cc and gdb)"
elif [[ $(cat /proc/sys/kernel/core_pattern 2>/dev/null) == '|'* ]]; then
  echo "skip end-to-end backtrace (cores are piped to a handler on this host)"
else
  cat >"$crash_dir/boom.c" <<'EOF'
static void crashing_frame(void) { *(volatile int *)0 = 1; }
int main(void) { crashing_frame(); return 0; }
EOF
  cc -g -O0 -o "$crash_dir/boom" "$crash_dir/boom.c"
  # Run the crash under its own shell: the shell that reaps a signalled child
  # announces it, and that announcement is this script's output otherwise. The
  # trailing [:] keeps that shell from exec'ing into ./boom, which would put
  # the crash back under this script and reinstate the announcement.
  bash -c "cd '$crash_dir' && ulimit -c unlimited && { ./boom; :; }" \
    >/dev/null 2>&1 || true
  # shellcheck disable=SC2012
  if [ -z "$(cd "$crash_dir" && ls -1t core core.* ./*.core 2>/dev/null)" ]; then
    echo "skip end-to-end backtrace (no core was written)"
  else
    out=$(bash -c "
set -Eeuo pipefail
declare -a CONTROL_LOG_RECORDS=()
. '$HELPERS'
# Stand in for ssh: the fragment is meant to run on the crashed process's host,
# and here that host is this one.
remote_run() { bash -c \"\$2\"; }
CONTROL_LOG_RECORDS+=('hog11|node1_mempool|/tmp/out|/tmp/err')
CRASH_CORE_SEARCH_DIRS=('$crash_dir')
replay_crash_backtrace_for_label node1_mempool 139
" 2>&1) && status=0 || status=$?
    expect_status 'end-to-end backtrace' 0 "$status"
    expect_contains 'end-to-end finds the core' "core file: $crash_dir/" "$out"
    expect_contains 'end-to-end names the crashing frame' 'crashing_frame' "$out"
    expect_contains 'end-to-end reports the signal' 'SIGSEGV' "$out"
    expect_lacks 'end-to-end resolves the binary' \
      'could not identify the binary' "$out"
  fi
fi

exit "$fail"
