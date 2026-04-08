# Get current branch name
if [[ -z ${GIT_BRANCH} ]]; then
  GIT_branch=$(git rev-parse --abbrev-ref HEAD)
else
  GIT_branch=${GIT_BRANCH}
fi

echo "GIT_branch: $GIT_branch"

# Get current commit hash
GIT_commit=$(git rev-parse HEAD)

# Get current commit timestamp in UTC ISO 8601 format (YYYY-MM-DDTHH:MM:SS)
GIT_commit_unix_time=$(git show -s --format=%ct HEAD)
if date -u -r "$GIT_commit_unix_time" "+%Y-%m-%dT%H:%M:%S" >/dev/null 2>&1; then
  # macOS/BSD date
  GIT_commit_time=$(date -u -r "$GIT_commit_unix_time" "+%Y-%m-%dT%H:%M:%S")
else
  # GNU date (Linux)
  GIT_commit_time=$(date -u -d "@$GIT_commit_unix_time" "+%Y-%m-%dT%H:%M:%S")
fi

echo "Branch: $GIT_branch"
echo "Commit: $GIT_commit"
echo "Commit Timestamp: $GIT_commit_time"
