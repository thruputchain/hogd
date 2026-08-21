#!/usr/bin/env bash
# Self-test for commit_metrics.sh's push loop, driven against real local
# repositories with real pre-receive hooks.
#
# The loop exists so that losing a race to another job's metrics commit is
# survivable: refresh the parent and push again. What it must not do is treat a
# permanent refusal the same way. A file over GitHub's size limit is rejected
# identically every time, and a loop that keeps trying turns that into a job
# that sits there until its schedule's timeout kills it -- which is how the
# combined-metrics schedule spent 29 minutes pushing a 127 MB chuckd log.
set -euo pipefail

SCRIPTS=$(cd "$(dirname "$(readlink -f "$0")")/../.." && pwd)/scripts
COMMIT_METRICS="$SCRIPTS/commit_metrics.sh"
fail=0
roots=()

cleanup() {
  local root
  for root in ${roots[@]+"${roots[@]}"}; do
    rm -rf "$root"
  done
}
trap cleanup EXIT

check() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: got $got, want $want"
    fail=1
  else
    echo "ok   $label"
  fi
}

expect_says() {
  local label=$1 needle=$2 haystack=$3
  if [[ "$haystack" != *"$needle"* ]]; then
    echo "FAIL $label: output missing [$needle]"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  else
    echo "ok   $label"
  fi
}

# Builds a "remote" holding a main branch, and a hogd-like working repo with a
# metrics_history/ checkout of it and one metric file staged to commit. Leaves
# $root set and the shell inside the working repo.
setup() {
  root=$(mktemp -d)
  roots+=("$root")

  git init -q --bare "$root/remote.git"

  # Seed main before any hook can refuse it.
  git init -q "$root/seed"
  git -C "$root/seed" -c user.email=s@x -c user.name=s commit -q --allow-empty -m base
  git -C "$root/seed" branch -M main
  git -C "$root/seed" push -q "$root/remote.git" main

  git init -q "$root/work"
  git -C "$root/work" -c user.email=s@x -c user.name=s commit -q --allow-empty -m hogd
  cd "$root/work"
  METRICS_HISTORY_URL="$root/remote.git" "$SCRIPTS/clone_metrics_history.sh" >/dev/null 2>&1
  mkdir -p metrics_history/metrics/some_metric/main
  echo 'a metric' >metrics_history/metrics/some_metric/main/2026-01-01T00:00:00_abc
}

# Installs a pre-receive hook. With an argument, the hook refuses only until
# that file exists, so the first push fails and the next one lands.
decline_pushes() {
  local once_until=${1:-}
  cat >"$root/remote.git/hooks/pre-receive" <<HOOK
#!/bin/sh
if [ -n "$once_until" ] && [ -f "$once_until" ]; then
    exit 0
fi
[ -z "$once_until" ] || touch "$once_until"
echo "remote: error: File huge.log is 127.04 MB; this exceeds GitHub's file size limit of 100.00 MB" >&2
exit 1
HOOK
  chmod +x "$root/remote.git/hooks/pre-receive"
}

retry_count() {
  printf '%s\n' "$1" | grep -c 'refreshing metrics_history parent' || true
}

landed_count() {
  git --git-dir="$root/remote.git" log --oneline main |
    grep -c 'Add metrics for hogd' || true
}

# A push refused for a permanent reason must stop, and must say why. The
# [timeout] is the assertion that matters most: before this was bounded, the
# loop ran until something outside it gave up.
setup
decline_pushes
out=$(timeout 120 "$COMMIT_METRICS" 2>&1) && status=0 || status=$?
check 'permanent rejection is bounded, not killed by a timeout' 1 "$status"
check 'permanent rejection retries exactly once' 1 "$(retry_count "$out")"
expect_says 'permanent rejection says a retry cannot help' 'no race was lost' "$out"
expect_says "permanent rejection keeps the remote's own reason" \
  'exceeds GitHub' "$out"
check 'permanent rejection lands nothing' 0 "$(landed_count)"

# A transient rejection -- what losing a race to a competing metrics job looks
# like from here -- is still retried, and still wins. The first failure always
# gets one retry; only a second identical failure is read as permanent.
setup
decline_pushes "$root/raced"
out=$(timeout 120 "$COMMIT_METRICS" 2>&1) && status=0 || status=$?
check 'transient rejection is retried and wins' 0 "$status"
check 'transient rejection retries once' 1 "$(retry_count "$out")"
check 'transient rejection lands the commit' 1 "$(landed_count)"

# The ordinary path is untouched.
setup
out=$(timeout 120 "$COMMIT_METRICS" 2>&1) && status=0 || status=$?
check 'a clean push succeeds' 0 "$status"
check 'a clean push retries nothing' 0 "$(retry_count "$out")"
check 'a clean push lands the commit' 1 "$(landed_count)"

# Nothing to publish is a no-op success, not a failure.
setup
rm -rf metrics_history/metrics
out=$(timeout 120 "$COMMIT_METRICS" 2>&1) && status=0 || status=$?
check 'no metric files exits 0' 0 "$status"
expect_says 'no metric files says so' 'No metric files found' "$out"
check 'no metric files lands nothing' 0 "$(landed_count)"

exit "$fail"
