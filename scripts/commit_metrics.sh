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

while true; do
  if ! prepare_metrics_commit; then
    exit 0
  fi

  if git push --quiet origin "HEAD:$METRICS_HISTORY_BRANCH"; then
    break
  else
    echo "Push failed, refreshing metrics_history parent and retrying..."
  fi
done
