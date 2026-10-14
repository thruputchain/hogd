#!/bin/bash
set -e

if [ $# -ne 4 ]; then
  echo "Usage: $0 <metric_name> <branch> <commit> <commit_time>" >&2
  exit 1
fi

METRIC_NAME=$1
BRANCH=$2

mkdir -p "metrics_history/metrics/$METRIC_NAME/$BRANCH"
