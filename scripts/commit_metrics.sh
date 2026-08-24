#!/bin/bash
set -e

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/git_info.sh
METRICS_HISTORY_BRANCH=${METRICS_HISTORY_BRANCH:-main}

# Push metrics_history/ to groundhogchain/metrics_history.git
cd metrics_history/

# Check if git committer name and email are set; if not, set them to metrics-script
committer_name=$(git config user.name || true)
committer_email=$(git config user.email || true)
if [ -z "$committer_name" ]; then
  committer_name="metrics-script"
fi
if [ -z "$committer_email" ]; then
  committer_email="metrics-script@groundhogchain.com"
fi
echo "Committer name: $committer_name"
echo "Committer email: $committer_email"

echo "Commit message: Add metrics for $GIT_branch $GIT_commit"

stage_present_metrics() {
  if [ ! -d metrics ] || ! find metrics -type f -print -quit | grep -q .; then
    return 1
  fi

  git read-tree HEAD
  while IFS= read -r -d '' metric_file; do
    git add -- "$metric_file"
  done < <(find metrics -type f -print0)
}

prepare_metrics_commit() {
  git fetch --depth=1 --filter=blob:none --quiet origin "$METRICS_HISTORY_BRANCH"
  git update-ref "refs/heads/$METRICS_HISTORY_BRANCH" FETCH_HEAD
  git symbolic-ref HEAD "refs/heads/$METRICS_HISTORY_BRANCH"

  if ! stage_present_metrics; then
    echo "No metric files found to commit."
    return 1
  fi

  if git diff --cached --quiet -- metrics/; then
    echo "No metrics changes to commit."
    return 1
  fi

  GIT_COMMITTER_NAME="$committer_name" \
    GIT_COMMITTER_EMAIL="$committer_email" \
    git commit -m "Add metrics for hogd $GIT_branch $GIT_commit" --author "$committer_name <$committer_email>" --quiet
}

# Refreshing the parent and re-pushing only ever helps for one failure mode: a
# concurrent CI run landed first, so our push is a non-fast-forward and
# [prepare_metrics_commit] rebuilds the commit on the refreshed parent. Every
# other rejection re-pushes byte-identical content and gets the same answer --
# most importantly a pre-receive hook declining an oversized file (GH001),
# where an unbounded loop becomes a hang: issue #3508 spent 22 minutes
# re-pushing a rejected 436 MiB log until the CI job's own timeout killed it,
# so the [|| true] at the call sites never got a chance to run.
#
# So retry only the race, and only a bounded number of times. Anything else
# fails fast with the remote's own output, which is what a human needs to see.
#
# Validate the override rather than trusting it: [ 1 -ge abc ] does not fail
# the script under [set -e] -- as an [if] condition it just reports an integer
# expression error and evaluates false -- so a malformed value would silently
# restore the unbounded loop this whole block exists to remove.
MAX_PUSH_ATTEMPTS=${MAX_PUSH_ATTEMPTS:-10}
case $MAX_PUSH_ATTEMPTS in
*[!0-9]* | '' | 0*)
  echo "MAX_PUSH_ATTEMPTS must be a positive integer; got '$MAX_PUSH_ATTEMPTS'." >&2
  exit 1
  ;;
esac
attempt=0

push_is_race() {
  printf '%s' "$1" | grep -qE 'non-fast-forward|fetch first|stale info'
}

while true; do
  if ! prepare_metrics_commit; then
    exit 0
  fi

  attempt=$((attempt + 1))
  push_output=$(git push --quiet origin "HEAD:$METRICS_HISTORY_BRANCH" 2>&1) && break
  printf '%s\n' "$push_output" >&2

  if ! push_is_race "$push_output"; then
    echo "Push was rejected for a reason retrying cannot fix. Giving up." >&2
    exit 1
  fi

  if [ "$attempt" -ge "$MAX_PUSH_ATTEMPTS" ]; then
    echo "Push lost the race with a concurrent run $attempt times. Giving up." >&2
    exit 1
  fi

  echo "Push failed, refreshing metrics_history parent and retrying..."
done
