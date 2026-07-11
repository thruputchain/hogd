#!/bin/bash
set -e

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/git_info.sh

if [ $# -ne 2 ]; then
  echo "Usage: $0 <metric_name> <filename_to_log>"
  exit 1
fi

METRIC_NAME=$1
FILENAME_TO_LOG=$2

./scripts/make_metric_file.sh "$METRIC_NAME" "$GIT_branch" "$GIT_commit" "$GIT_commit_time"
cat "$FILENAME_TO_LOG" >"metrics_history/metrics/$METRIC_NAME/$GIT_branch/${GIT_commit_time}_$GIT_commit"
