#!/bin/sh

if [ $# -ne 1 ]; then
    echo "Usage: $0 <build_dir>"
    exit 1
fi

set -e

BUILD_DIR=$1

# Get current branch name
if [[ -z ${GITHUB_HEAD_REF} ]]; then
    branch=$(git rev-parse --abbrev-ref HEAD)
else
    branch=${GITHUB_HEAD_REF}
fi

# Get current commit hash
commit=$(git rev-parse HEAD)

# Get current commit timestamp in ISO 8601 format (YYYY-MM-DDTHH:MM:SS)
commit_time=$(TZ=UTC git show -s --format=%cd --date=format:'%Y-%m-%dT%H:%M:%S' HEAD)

# Check if the working directory is dirty (tracked files only)
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
    echo "Working directory is dirty; cannot replicably run metrics"
    exit 1
fi

echo "Branch: $branch"
echo "Commit: $commit"
echo "Commit Timestamp: $commit_time"

# Check if metrics_history/ is a git repo; if not, clone it
if [ ! -d "metrics_history/.git" ]; then
    echo "Cloning groundhogchain/metrics_history.git into metrics_history/"
    git clone https://github.com/groundhogchain/metrics_history.git metrics_history/
else
    echo "metrics_history/ already exists and is a git repository."
fi

# Put metrics to run here
./metrics_history/scripts/make_metric_file.sh fibonacci-codesize $branch $commit $commit_time
./scripts/metrics/fibonacci_codesize.sh $BUILD_DIR > metrics_history/metrics/fibonacci-codesize/$branch/${commit_time}_$commit

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

git add metrics/
GIT_COMMITTER_NAME="$committer_name" \
GIT_COMMITTER_EMAIL="$committer_email" \
git commit -m "Add metrics for $branch $commit" --author "$committer_name <$committer_email>"
git push












