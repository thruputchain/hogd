#!/usr/bin/env bash
# Self-test for commit_metrics.sh, against a local metrics_history remote.
#
# The case that matters most here is the stalled push: the script must end on
# it rather than start another round, because a round that re-stalls spends
# the whole per-attempt bound again and a few of those are most of an
# hour-long job's budget.
set -euo pipefail

SCRIPT="$(dirname "$(readlink -f "$0")")/commit_metrics.sh"
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

expect_eq() {
  local label=$1 want=$2 got=$3
  if [[ "$got" != "$want" ]]; then
    echo "FAIL $label: got [$got], want [$want]"
    fail=1
  else
    echo "ok   $label"
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

git init -q --bare "$work/remote.git" -b main
git init -q "$work/seed" -b main
(
  cd "$work/seed"
  mkdir -p metrics
  echo historical >metrics/old.json
  git add -A
  git -c user.name=seed -c user.email=seed@test commit -qm init
  git push -q "$work/remote.git" main 2>/dev/null
)

# The repository the script is invoked from: git_info.sh reads the branch and
# commit being reported on from it.
git init -q "$work/hogd" -b main
(
  cd "$work/hogd"
  echo source >file
  git add -A
  git -c user.name=seed -c user.email=seed@test commit -qm hogd
)
cd "$work/hogd"
METRICS_HISTORY_URL="$work/remote.git" \
  "$(dirname "$SCRIPT")/clone_metrics_history.sh" >/dev/null 2>&1

new_metric() {
  mkdir -p metrics_history/metrics
  echo "$1" >"metrics_history/metrics/$1.json"
}

status=0
new_metric run1
out=$("$SCRIPT" 2>&1) || status=$?
expect_status 'a run with new metrics' 0 "$status"
expect_eq 'the commit reaches the remote' \
  'metrics/old.json metrics/run1.json' \
  "$(git -C "$work/remote.git" ls-tree -r --name-only main | tr '\n' ' ' | sed 's/ $//')"

# Nothing new staged, so there is nothing to file and that is not a failure.
status=0
out=$("$SCRIPT" 2>&1) || status=$?
expect_status 'a run with nothing new' 0 "$status"
expect_contains 'a run with nothing new says so' 'No metrics changes to commit.' "$out"

# A push that never answers. [pushurl] sends only the push through an ssh that
# hangs, leaving the fetch on the local path, so this is a stalled push and
# not a remote that is gone.
ssh_log="$work/ssh-calls"
cat >"$work/hang-ssh" <<HANG
#!/usr/bin/env bash
echo call >>"$ssh_log"
sleep 600
HANG
chmod +x "$work/hang-ssh"
: >"$ssh_log"
git -C metrics_history config remote.origin.pushurl "ssh://git@localhost/metrics_history.git"
new_metric run2

status=0
out=$(
  GIT_SSH_COMMAND="$work/hang-ssh" \
    METRICS_HISTORY_GIT_TIMEOUT_SEC=1 \
    METRICS_HISTORY_GIT_ATTEMPTS=2 \
    METRICS_HISTORY_GIT_KILL_AFTER=1s \
    "$SCRIPT" 2>&1
) || status=$?
expect_status 'a push that stalls' 1 "$status"
expect_contains 'a stalled push says so' 'Push to metrics_history stalled' "$out"
# Two attempts inside one metrics_history_git call, and then the script stops.
# Any more means a stall started another round of the outer loop, which is
# what makes a stalling remote outlast the job.
expect_eq 'a stalled push does not start another round' 2 "$(wc -l <"$ssh_log")"

git -C metrics_history config --unset remote.origin.pushurl

# A file the remote will not take must not take the run's other metrics with
# it. GitHub rejects an oversized file at the pre-receive hook and the whole
# push with it, and a run's metrics are one commit, so staging it loses every
# metric the run produced -- silently, since the schedules call this as
# [(commit_metrics.sh) || true].
new_metric run3
printf '%0.sx' $(seq 1 4096) >metrics_history/metrics/oversized.log

status=0
out=$(METRICS_MAX_FILE_BYTES=1024 "$SCRIPT" 2>&1) || status=$?
expect_status 'a run with an oversized metric file' 0 "$status"
expect_contains 'an oversized metric file is named' \
  'Skipping metrics/oversized.log' "$out"
expect_eq "the run's other metrics still reach the remote" \
  'metrics/old.json metrics/run1.json metrics/run2.json metrics/run3.json' \
  "$(git -C "$work/remote.git" ls-tree -r --name-only main | tr '\n' ' ' | sed 's/ $//')"
rm -f metrics_history/metrics/oversized.log

# A remote that cannot be reached at all is reported, not mistaken for a run
# with no metrics to file.
git -C metrics_history remote set-url origin "$work/absent.git"
status=0
out=$("$SCRIPT" 2>&1) || status=$?
expect_status 'a fetch that fails' 1 "$status"
expect_contains 'a failed fetch says so' 'Could not fetch main from metrics_history.' "$out"

if [[ "$fail" -ne 0 ]]; then
  echo "commit_metrics self-test FAILED"
  exit 1
fi
echo "commit_metrics self-test passed"
