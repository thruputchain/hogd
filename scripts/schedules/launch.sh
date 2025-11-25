#!/bin/sh
set -e

if [ $# -ne 2 ]; then
  echo "Usage: $0 <xid> <schedule_toml>"
  exit 1
fi

SCHED_DIR=$(dirname $(readlink -f "$2"))

github_repo() {
    dir="$1"

    # If no directory provided, default to current directory
    [ -n "$dir" ] || dir="."

    # Get URL from the target repo
    url=$(git -C "$dir" remote get-url origin 2>/dev/null || echo "")

    # Strip GitHub prefixes (SSH or HTTPS)
    url=${url#git@github.com:}
    url=${url#https://github.com/}
    url=${url#http://github.com/}

    # Strip trailing .git if present
    url=${url%.git}

    printf '%s\n' "$url"
}

git_commit_sha() {
    dir="$1"

    # Default to current directory if none given
    [ -n "$dir" ] || dir="."

    # Query HEAD SHA
    sha=$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo "")

    printf '%s\n' "$sha"
}

git_current_branch() {
    dir="$1"

    # Default to current directory if none given
    [ -n "$dir" ] || dir="."

    # Get the branch name (empty if detached)
    branch=$(git -C "$dir" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")

    printf '%s\n' "$branch"
}

hog_sched -x=$1 \
    --key=github_repo --value=$(github_repo $SCHED_DIR) \
    --key=git_commit --value=$(git_commit_sha $SCHED_DIR) \
    --key=git_branch --value=$(git_current_branch $SCHED_DIR) \
    $2
