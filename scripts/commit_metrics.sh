#!/bin/bash
set -e

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/git_info.sh

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

git add metrics/

if git diff --cached --quiet -- metrics/; then
  echo "No metrics changes to commit."
  exit 0
fi

GIT_COMMITTER_NAME="$committer_name" \
  GIT_COMMITTER_EMAIL="$committer_email" \
  git commit -m "Add metrics for hogd $GIT_branch $GIT_commit" --author "$committer_name <$committer_email>" --quiet

while true; do
  if git push --quiet; then
    break
  else
    echo "Push failed, attempting pull --rebase and retry..."
    GIT_COMMITTER_NAME="$committer_name" \
      GIT_COMMITTER_EMAIL="$committer_email" \
      git pull --rebase --quiet
  fi
done
