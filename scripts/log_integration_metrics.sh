#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <coordinator_output_file>"
  exit 1
fi

set -e

COORDINATOR_OUTPUT_FILE=$1

# Get current branch name
if [[ -z ${GITHUB_HEAD_REF} ]]; then
  branch=$(git rev-parse --abbrev-ref HEAD)
else
  branch=${GITHUB_HEAD_REF}
fi

# Get current commit hash
commit=$(git rev-parse HEAD)

# Get current commit timestamp in UTC ISO 8601 format (YYYY-MM-DDTHH:MM:SS)
commit_unix_time=$(git show -s --format=%ct HEAD)
if date -u -r "$commit_unix_time" "+%Y-%m-%dT%H:%M:%S" >/dev/null 2>&1; then
  # macOS/BSD date
  commit_time=$(date -u -r "$commit_unix_time" "+%Y-%m-%dT%H:%M:%S")
else
  # GNU date (Linux)
  commit_time=$(date -u -d "@$commit_unix_time" "+%Y-%m-%dT%H:%M:%S")
fi

echo "Branch: $branch"
echo "Commit: $commit"
echo "Commit Timestamp: $commit_time"

./scripts/clone_metrics_history.sh

# Log integration metrics

./scripts/make_metric_file.sh nightly_integration_mempool "$branch" "$commit" "$commit_time"
cat "$COORDINATOR_OUTPUT_FILE" >"metrics_history/metrics/nightly_integration_mempool/$branch/${commit_time}_$commit"

./scripts/commit_metrics.sh
