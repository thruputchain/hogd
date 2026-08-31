#!/bin/bash
set -e

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/git_info.sh

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  echo "Usage: $0 <metric_name> <filename_to_log> [sample_time]"
  exit 1
fi

METRIC_NAME=$1
FILENAME_TO_LOG=$2

# The time this sample is filed under. Defaults to the commit's, which is right
# for a metric produced once per commit: one file per commit, named after it.
#
# A metric measured repeatedly on one commit needs the measurement time instead.
# The gas calibration is the case -- it re-runs nightly whether or not main has
# advanced, and every run is an independent sample of the machine -- and under
# the commit's time each run would overwrite the last, leaving one sample per
# commit rather than one per night. Same shape either way, so the filename stays
# <time>_<commit> and still sorts chronologically.
SAMPLE_TIME=${3:-$GIT_commit_time}

./scripts/make_metric_file.sh "$METRIC_NAME" "$GIT_branch" "$GIT_commit" "$GIT_commit_time"
cat "$FILENAME_TO_LOG" >"metrics_history/metrics/$METRIC_NAME/$GIT_branch/${SAMPLE_TIME}_$GIT_commit"
