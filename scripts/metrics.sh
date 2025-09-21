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

# Get current commit timestamp in UTC ISO 8601 format (YYYY-MM-DDTHH:MM:SS)
commit_unix_time=$(git show -s --format=%ct HEAD)
if date -u -r "$commit_unix_time" "+%Y-%m-%dT%H:%M:%S" >/dev/null 2>&1; then
  # macOS/BSD date
  commit_time=$(date -u -r "$commit_unix_time" "+%Y-%m-%dT%H:%M:%S")
else
  # GNU date (Linux)
  commit_time=$(date -u -d "@$commit_unix_time" "+%Y-%m-%dT%H:%M:%S")
fi

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

# Codesize metrics
## fibonaci.wasm
./metrics_history/scripts/make_metric_file.sh fibonacci-codesize $branch $commit $commit_time
./scripts/metrics/codesize.sh $BUILD_DIR fibonacci >metrics_history/metrics/fibonacci-codesize/$branch/${commit_time}_$commit

## itc_erc20.wasm
./metrics_history/scripts/make_metric_file.sh erc20-codesize $branch $commit $commit_time
./scripts/metrics/codesize.sh $BUILD_DIR itc_erc20 >metrics_history/metrics/erc20-codesize/$branch/${commit_time}_$commit

## initial_token_claim.wasm
./metrics_history/scripts/make_metric_file.sh initial_token_claim-codesize $branch $commit $commit_time
./scripts/metrics/codesize.sh $BUILD_DIR initial_token_claim >metrics_history/metrics/initial_token_claim-codesize/$branch/${commit_time}_$commit

# Performance metrics
## hogdb_bench
./metrics_history/scripts/make_metric_file.sh hogdb-bench $branch $commit $commit_time
./scripts/metrics/hogdb_bench.sh $BUILD_DIR >metrics_history/metrics/hogdb-bench/$branch/${commit_time}_$commit

## mass fibonacci.wasm calls
./metrics_history/scripts/make_metric_file.sh fibonacci-wasmonly $branch $commit $commit_time
./scripts/metrics/fibonacci_wasmonly_bench.sh $BUILD_DIR >metrics_history/metrics/fibonacci-wasmonly/$branch/${commit_time}_$commit

## payments_bench 10 accounts
./metrics_history/scripts/make_metric_file.sh payments-bench-10 $branch $commit $commit_time
./scripts/metrics/payments_bench.sh $BUILD_DIR 10 >metrics_history/metrics/payments-bench-10/$branch/${commit_time}_$commit

# shut down the other CI control server
$BUILD_DIR/hogd/hogexec/bench/control_cli hog11 11111 shutdown

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
