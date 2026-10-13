#!/bin/bash
# Bounded git network access to metrics_history, for the scripts that clone it
# and commit into it.
#
# Those scripts run inside hog_sched jobs whose only time bound is the
# schedule's --timeout-sec for the run as a whole, and git has no network
# timeout of its own: a transfer that stalls after the remote has finished
# packing waits forever. Combined Metrics run #2375 lost the last 19 minutes of
# its hour that way, stalled in [git fetch] of metrics_history, and the
# schedule killed every host script mid-fetch -- failing a job whose benchmarks
# had all passed, in a commit step the schedule had deliberately made
# best-effort with [|| true].
#
# Bound each attempt instead. A stall then costs an attempt rather than the
# rest of the job's budget, and the caller's own error handling gets to run.

# Per-attempt wall clock. The remote spends a couple of minutes enumerating and
# compressing this repository's trees before any of the pack arrives, so this
# leaves room for a slow fetch and trips only on a genuine stall.
METRICS_HISTORY_GIT_TIMEOUT_SEC=${METRICS_HISTORY_GIT_TIMEOUT_SEC:-300}
# A stalled attempt is retried once. A transfer that stalls twice is not going
# to finish inside what is left of the job either.
METRICS_HISTORY_GIT_ATTEMPTS=${METRICS_HISTORY_GIT_ATTEMPTS:-2}
# How long a git that ignores SIGTERM while blocked on the network gets before
# the SIGKILL. Configurable so the self-test need not wait out the default.
METRICS_HISTORY_GIT_KILL_AFTER=${METRICS_HISTORY_GIT_KILL_AFTER:-30s}

# What metrics_history_git returns when every attempt stalled, as opposed to a
# git that ran and reported something. [timeout] exits 124 when its own
# SIGTERM stopped the command and 137 when the SIGKILL that follows did; both
# mean the same thing to a caller, so both arrive here as this one status.
METRICS_HISTORY_GIT_STALLED=124

# Run [git "$@"] under the per-attempt bound, retrying a stall and nothing
# else. Any other failure is the caller's to interpret: commit_metrics.sh, for
# one, reads a rejected push as "another job pushed first, refresh the parent
# and build a new commit", which a blind retry here would turn into offering
# the same rejected commit twice.
metrics_history_git() {
  local attempt=1 status

  while true; do
    status=0
    timeout --kill-after="$METRICS_HISTORY_GIT_KILL_AFTER" \
      "$METRICS_HISTORY_GIT_TIMEOUT_SEC" git "$@" || status=$?

    if [ "$status" -eq 0 ]; then
      return 0
    fi
    if [ "$status" -ne 124 ] && [ "$status" -ne 137 ]; then
      return "$status"
    fi

    echo "metrics_history: [git $*] stalled for ${METRICS_HISTORY_GIT_TIMEOUT_SEC}s" \
      "(attempt ${attempt}/${METRICS_HISTORY_GIT_ATTEMPTS})" >&2

    if [ "$attempt" -ge "$METRICS_HISTORY_GIT_ATTEMPTS" ]; then
      return "$METRICS_HISTORY_GIT_STALLED"
    fi
    attempt=$((attempt + 1))
  done
}
