#!/bin/bash
set -e

METRICS_HISTORY_BRANCH=${METRICS_HISTORY_BRANCH:-main}
METRICS_HISTORY_URL=${METRICS_HISTORY_URL:-https://github.com/groundhogchain/metrics_history.git}

if [ -e "metrics_history" ] && [ ! -d "metrics_history/.git" ]; then
  echo "metrics_history/ exists but is not a git repository." >&2
  exit 1
fi

if [ ! -d "metrics_history/.git" ]; then
  echo "Initializing shallow, blobless metrics_history/ from ${METRICS_HISTORY_URL}"
  mkdir -p metrics_history
  git -C metrics_history init --quiet
  git -C metrics_history remote add origin "$METRICS_HISTORY_URL"
else
  echo "metrics_history/ already exists and is a git repository."
fi

git -C metrics_history fetch --depth=1 --filter=blob:none --quiet origin "$METRICS_HISTORY_BRANCH"
git -C metrics_history update-ref "refs/heads/$METRICS_HISTORY_BRANCH" FETCH_HEAD
git -C metrics_history symbolic-ref HEAD "refs/heads/$METRICS_HISTORY_BRANCH"
git -C metrics_history config "branch.$METRICS_HISTORY_BRANCH.remote" origin
git -C metrics_history config "branch.$METRICS_HISTORY_BRANCH.merge" "refs/heads/$METRICS_HISTORY_BRANCH"

# Populate the index from the remote tip without checking out the historical
# metrics files. Metric writers create only the new files for this CI run.
git -C metrics_history read-tree HEAD
