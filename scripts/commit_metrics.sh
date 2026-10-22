#!/bin/bash
set -e

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
source ${SCRIPT_DIR}/git_info.sh
source ${SCRIPT_DIR}/metrics_history_git.sh
METRICS_HISTORY_BRANCH=${METRICS_HISTORY_BRANCH:-main}
# The repo token in the commit message. metrics_history's alert workflows
# (scripts/parse_commit.sh there) read the last three tokens as
# "<repo> <branch> <commit>" and only check commits whose repo is exactly
# "hogd", so a pipeline that files a partial set of metrics on purpose sets
# this to something else to stay out of the missing-metrics alerts. Must not
# contain spaces.
METRICS_COMMIT_REPO=${METRICS_COMMIT_REPO:-hogd}
# How many times to build and offer a commit before giving up. A round is lost
# only when another job pushes between our fetch and our push, so the handful
# below is plenty; the point of the bound is that the loop ends at all. An
# unbounded one is how a network that never recovers turns a best-effort
# metrics commit into a job that runs until the schedule kills it.
#
# Rounds are cheap only because a stall never starts one: both the fetch and
# the push below end the script the moment they stall, so at most one stall
# happens here and the rounds that do repeat are a remote answering quickly
# with "no". Without that, five rounds of a stalling push would be fifty
# minutes of an hour-long job -- the very failure this script is being fixed
# for, one loop further out.
METRICS_COMMIT_ATTEMPTS=${METRICS_COMMIT_ATTEMPTS:-5}

# The largest metric file this will offer to the remote. GitHub rejects a file
# over 100 MiB at the pre-receive hook, and the whole push with it. A run's
# metrics are one commit, so an oversized file does not fail on its own: it
# takes every metric the run produced with it, on each of the attempts above
# and then silently, because the schedules call this as
# [(commit_metrics.sh) || true]. Leaving the file out costs that one file.
#
# This is a backstop and not the place to fix a producer: a pipeline that
# writes a file this large should bound it at the source, the way
# [ci_copy_remote_log_capped] does for the node logs that first hit this.
METRICS_MAX_FILE_BYTES=${METRICS_MAX_FILE_BYTES:-$((100 * 1024 * 1024))}

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

echo "Commit message: Add metrics for $METRICS_COMMIT_REPO $GIT_branch $GIT_commit"

stage_present_metrics() {
  local metric_file
  local size

  if [ ! -d metrics ] || ! find metrics -type f -print -quit | grep -q .; then
    return 1
  fi

  git read-tree HEAD
  while IFS= read -r -d '' metric_file; do
    # Redirected rather than given the name, so the count is all that is
    # read back, and stripped because not every [wc] leaves it unpadded.
    size=$(wc -c <"$metric_file" | tr -cd '0-9')
    # A file whose size could not be read is staged rather than dropped: this
    # guard exists to save the run's other metrics from one oversized file,
    # not to decide which files get filed.
    if [ -n "$size" ] && [ "$size" -ge "$METRICS_MAX_FILE_BYTES" ]; then
      echo "Skipping $metric_file: $size bytes, at or over the" \
        "$METRICS_MAX_FILE_BYTES the remote accepts. The rest of this run's" \
        "metrics are filed without it." >&2
      continue
    fi
    git add -- "$metric_file"
  done < <(find metrics -type f -print0)
}

# Returns 0 having staged and committed this run's metrics, 1 when there is
# nothing to commit, and 2 when metrics_history could not be reached. The
# caller has to tell "no metrics this run" apart from "we never got the
# parent"; both used to leave this function the same way, and the second is
# reachable now that a stalled fetch ends in a failure rather than a hang.
# [set -e] does not apply here -- this is called from a conditional -- so each
# step that the rest depends on is checked by hand.
prepare_metrics_commit() {
  if ! metrics_history_git fetch --depth=1 --filter=blob:none --quiet \
    origin "$METRICS_HISTORY_BRANCH"; then
    echo "Could not fetch $METRICS_HISTORY_BRANCH from metrics_history." >&2
    return 2
  fi
  git update-ref "refs/heads/$METRICS_HISTORY_BRANCH" FETCH_HEAD || return 2
  git symbolic-ref HEAD "refs/heads/$METRICS_HISTORY_BRANCH" || return 2

  if ! stage_present_metrics; then
    echo "No metric files found to commit."
    return 1
  fi

  if git diff --cached --quiet -- metrics/; then
    echo "No metrics changes to commit."
    return 1
  fi

  GIT_COMMITTER_NAME="$committer_name" \
    GIT_COMMITTER_EMAIL="$committer_email" \
    git commit -m "Add metrics for $METRICS_COMMIT_REPO $GIT_branch $GIT_commit" --author "$committer_name <$committer_email>" --quiet || return 2
}

attempt=1
while true; do
  prepare_status=0
  prepare_metrics_commit || prepare_status=$?
  case "$prepare_status" in
  0) ;;
  1) exit 0 ;;
  *) exit 1 ;;
  esac

  push_status=0
  metrics_history_git push --quiet origin "HEAD:$METRICS_HISTORY_BRANCH" ||
    push_status=$?
  if [ "$push_status" -eq 0 ]; then
    break
  fi

  # A stalled push ends the script, the way a stalled fetch does. Another
  # round would spend the same bound again on the same silent remote, and the
  # commit is best-effort; only a push the remote answered is worth retrying.
  if [ "$push_status" -eq "$METRICS_HISTORY_GIT_STALLED" ]; then
    echo "Push to metrics_history stalled; giving up." >&2
    exit 1
  fi

  if [ "$attempt" -ge "$METRICS_COMMIT_ATTEMPTS" ]; then
    echo "Push failed on $METRICS_COMMIT_ATTEMPTS attempts; giving up." >&2
    exit 1
  fi
  echo "Push failed, refreshing metrics_history parent and retrying..."
  attempt=$((attempt + 1))
done
