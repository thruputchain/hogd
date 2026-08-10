#!/usr/bin/env bash
# Self-test for check_bench_hosts.sh: the hosts come from the schedule, a host
# with no forwarded agent is named and fatal, and every other verdict is
# reported without failing a run that hog_sched could still have made.
set -uo pipefail

CHECK=$(dirname "$(readlink -f "$0")")/check_bench_hosts.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
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

# A schedule shaped like the real ones: two jobs, one [jobs.host] table each,
# with a decoy quoted string outside them that must not be read as a host.
cat >"$work/schedule.toml" <<'TOML'
name = "selftest"

[[jobs]]
script = """
echo "hognope"
"""

[jobs.env]
GIT_REPO = "hognope"

[jobs.host]
bench_1 = "hog5"
# bench_2 = "hognope"
bench_2 = "hog6"

[[jobs]]
[jobs.host]
metrics_1 = "hog10"
TOML

# Stands in for ssh. Each host's exit status comes from $STATUS_<host>, so a
# case can give every host a different verdict; the remote command is ignored.
cat >"$work/ssh" <<'FAKE'
#!/usr/bin/env bash
host=""
for arg in "$@"; do
  case $arg in
  -*) continue ;;
  hog*)
    host=$arg
    break
    ;;
  esac
done
var="STATUS_${host}"
exit "${!var:-0}"
FAKE
chmod +x "$work/ssh"

# Stands in for the runner's own ssh-add, whose reachable agent is the
# precondition for asking anything of the hosts.
cat >"$work/ssh-add" <<'FAKE'
#!/usr/bin/env bash
exit "${STATUS_LOCAL_AGENT:-0}"
FAKE
chmod +x "$work/ssh-add"

run_check() {
  env CHECK_BENCH_HOSTS_SSH="$work/ssh" \
    CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
    CHECK_BENCH_HOSTS_RETRY_DELAY_SEC=0 \
    "$@" "$CHECK" "$work/schedule.toml" 2>&1
}

# Every host healthy: the check names the hosts it read from the schedule, and
# says nothing about anything else.
out=$(run_check)
status=$?
expect_status 'all hosts ready' 0 "$status"
expect_contains 'names the host count' '3 hosts ready' "$out"
expect_contains 'names the hosts' 'hog10 hog5 hog6' "$out"
expect_lacks 'ignores strings outside [jobs.host]' 'hognope' "$out"

# The condition that kills hog_fwd: named, attributed, and fatal.
out=$(run_check STATUS_hog5=2)
status=$?
expect_status 'a host with no forwarded agent fails' 1 "$status"
expect_contains 'names the broken host' 'bench host hog5:' "$out"
expect_contains 'names the failure hog_fwd would print' \
  'Could not open a connection to your authentication agent.' "$out"
expect_contains 'counts the broken hosts against the schedule' \
  '1 of 3 hosts cannot run schedule.toml: hog5' "$out"
expect_contains 'says whose fault it is' 'host fault' "$out"
expect_lacks 'does not also claim the hosts are ready' 'hosts ready' "$out"

# More than one broken host is reported in full, not just the first.
out=$(run_check STATUS_hog5=2 STATUS_hog10=2)
status=$?
expect_status 'two hosts with no forwarded agent fail' 1 "$status"
expect_contains 'names both broken hosts' \
  '2 of 3 hosts cannot run schedule.toml: hog10 hog5' "$out"

# A host this check cannot reach may be this check's fault, so it is reported
# and left to hog_sched.
out=$(run_check STATUS_hog6=255)
status=$?
expect_status 'an unreachable host does not fail the run' 0 "$status"
expect_contains 'reports the unreachable host' 'bench host hog6:' "$out"
expect_contains 'explains why it is tolerated' 'could not be reached' "$out"
expect_contains 'still reports the hosts as ready' '3 hosts ready' "$out"
expect_contains 'says it continued anyway' \
  "could not clear: hog6" "$out"

# An agent with no identities may still be enough for hog_fwd, so it is
# reported without ending the run.
out=$(run_check STATUS_hog10=1)
status=$?
expect_status 'an empty agent does not fail the run' 0 "$status"
expect_contains 'reports the empty agent' 'holds no identities' "$out"

# A runner with no agent of its own says so, instead of blaming eight hosts.
out=$(run_check STATUS_LOCAL_AGENT=2)
status=$?
expect_status 'no local agent fails' 1 "$status"
expect_contains 'blames the runner, not the hosts' \
  'this runner has no reachable ssh-agent of its own' "$out"
expect_lacks 'does not name any host' 'bench host hog' "$out"

# A local agent that is reachable but empty is not this check's business.
out=$(run_check STATUS_LOCAL_AGENT=1)
status=$?
expect_status 'an empty local agent is not fatal' 0 "$status"
expect_contains 'an empty local agent still checks the hosts' \
  '3 hosts ready' "$out"

# A transient failure is not a broken host: the retry decides.
cat >"$work/flaky-ssh" <<'FAKE'
#!/usr/bin/env bash
marker=$FLAKY_MARKER
if [ ! -e "$marker" ]; then
  : >"$marker"
  exit 255
fi
exit 0
FAKE
chmod +x "$work/flaky-ssh"
out=$(env CHECK_BENCH_HOSTS_SSH="$work/flaky-ssh" \
  CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
  CHECK_BENCH_HOSTS_RETRY_DELAY_SEC=0 \
  FLAKY_MARKER="$work/flaky.marker" \
  CHECK_BENCH_HOSTS_ATTEMPTS=2 \
  "$CHECK" "$work/schedule.toml" 2>&1)
status=$?
expect_status 'a first-attempt failure is retried' 0 "$status"
expect_contains 'the retry clears the host' '3 hosts ready' "$out"

# Usage errors are usage errors, not host verdicts.
out=$(env CHECK_BENCH_HOSTS_SSH="$work/ssh" CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
  "$CHECK" "$work/nonexistent.toml" 2>&1)
status=$?
expect_status 'a missing schedule is a usage error' 2 "$status"
expect_contains 'names the missing schedule' 'no such schedule' "$out"

out=$(env CHECK_BENCH_HOSTS_SSH="$work/ssh" CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
  "$CHECK" 2>&1)
status=$?
expect_status 'no argument is a usage error' 2 "$status"
expect_contains 'prints usage' 'usage: check_bench_hosts.sh' "$out"

# A host the schedule leaves to hog_sched ("?") cannot be checked ahead of the
# run, so it is counted and skipped rather than treated as a hostname.
cat >"$work/wildcard.toml" <<'TOML'
[[jobs]]
[jobs.host]
bench_1 = "hog5"
bench_2 = "?"
TOML
out=$(env CHECK_BENCH_HOSTS_SSH="$work/ssh" CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
  "$CHECK" "$work/wildcard.toml" 2>&1)
status=$?
expect_status 'a wildcard alongside named hosts' 0 "$status"
expect_contains 'checks only the named host' '1 hosts ready' "$out"
expect_contains 'counts the wildcard' '1 further host(s) are chosen' "$out"
expect_lacks 'never treats "?" as a hostname' 'bench host ?' "$out"

# A schedule that names no host at all is not a broken schedule.
cat >"$work/all-wildcard.toml" <<'TOML'
[[jobs]]
[jobs.host]
tsan_1 = "?"
TOML
out=$(env CHECK_BENCH_HOSTS_SSH="$work/ssh" CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
  "$CHECK" "$work/all-wildcard.toml" 2>&1)
status=$?
expect_status 'an all-wildcard schedule is not an error' 0 "$status"
expect_contains 'says there is nothing to check' 'nothing to check' "$out"

: >"$work/empty.toml"
out=$(env CHECK_BENCH_HOSTS_SSH="$work/ssh" CHECK_BENCH_HOSTS_SSH_ADD="$work/ssh-add" \
  "$CHECK" "$work/empty.toml" 2>&1)
status=$?
expect_status 'a schedule with no hosts is a usage error' 2 "$status"
expect_contains 'says the schedule names no hosts' 'names no hosts' "$out"

exit "$fail"
