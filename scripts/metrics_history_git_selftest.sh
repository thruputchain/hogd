#!/usr/bin/env bash
# Self-test for metrics_history_git: a stalled git attempt must end in a
# failure the caller can act on rather than a wait with no end, and a git that
# failed for its own reasons must come back unretried and with its own status.
set -euo pipefail

HELPERS="$(dirname "$(readlink -f "$0")")/metrics_history_git.sh"
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

expect_calls() {
  local label=$1 want=$2 log=$3 got
  got=$(wc -l <"$log")
  if [[ "$got" -ne "$want" ]]; then
    echo "FAIL $label: git ran $got times, want $want"
    fail=1
  else
    echo "ok   $label call count"
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

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# A git whose behaviour per call comes from GIT_FAKE_SCRIPT: one word per call,
# [ok], [stall], or an exit status to report. Every call is recorded, so a test
# can tell one attempt from a retry.
cat >"$work/git" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >> "$GIT_FAKE_LOG"
read -r -a behaviours <<< "$GIT_FAKE_SCRIPT"
call=$(wc -l < "$GIT_FAKE_LOG")
behaviour=${behaviours[$((call - 1))]:-${behaviours[-1]}}
case "$behaviour" in
  ok) exit 0 ;;
  stall) sleep 600 ;;
  # A git wedged in a network call can sit through the SIGTERM; only the
  # SIGKILL from --kill-after stops it, and that reports a different status.
  deaf) trap '' TERM; sleep 600 ;;
  *) exit "$behaviour" ;;
esac
FAKE
chmod +x "$work/git"

run_case() {
  local script=$1
  shift
  : >"$work/log"
  (
    PATH="$work:$PATH"
    GIT_FAKE_LOG="$work/log"
    GIT_FAKE_SCRIPT="$script"
    export PATH GIT_FAKE_LOG GIT_FAKE_SCRIPT
    # Short enough to keep the self-test quick; the stall path is the only one
    # that waits for it.
    METRICS_HISTORY_GIT_TIMEOUT_SEC=1
    METRICS_HISTORY_GIT_ATTEMPTS=2
    METRICS_HISTORY_GIT_KILL_AFTER=1s
    # shellcheck source=/dev/null
    source "$HELPERS"
    metrics_history_git "$@"
  ) 2>&1
}

status=0
out=$(run_case 'ok' fetch --quiet origin main) || status=$?
expect_status 'a fetch that works' 0 "$status"
expect_calls 'a fetch that works' 1 "$work/log"

status=0
out=$(run_case 'stall' fetch --quiet origin main) || status=$?
expect_status 'a fetch that stalls' 124 "$status"
expect_calls 'a fetch that stalls' 2 "$work/log"
expect_contains 'a stall names itself' 'stalled for 1s' "$out"
expect_contains 'a stall names the command' '[git fetch --quiet origin main]' "$out"

status=0
out=$(run_case 'stall ok' fetch --quiet origin main) || status=$?
expect_status 'a stall that clears on the retry' 0 "$status"
expect_calls 'a stall that clears on the retry' 2 "$work/log"

# commit_metrics.sh reads the stall status to decide whether another round is
# worth starting, so a git that had to be killed must not arrive wearing a
# different number than one that stopped on the SIGTERM.
status=0
out=$(run_case 'deaf' fetch --quiet origin main) || status=$?
expect_status 'a fetch killed after ignoring SIGTERM' 124 "$status"
expect_calls 'a fetch killed after ignoring SIGTERM' 2 "$work/log"

# A rejected push is the caller's to handle -- commit_metrics.sh answers it by
# refreshing the parent -- so it must arrive unretried and with git's status.
status=0
out=$(run_case '128' push --quiet origin HEAD:main) || status=$?
expect_status 'a push git rejects' 128 "$status"
expect_calls 'a push git rejects' 1 "$work/log"

if [[ "$fail" -ne 0 ]]; then
  echo "metrics_history_git self-test FAILED"
  exit 1
fi
echo "metrics_history_git self-test passed"
