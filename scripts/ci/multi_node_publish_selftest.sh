#!/usr/bin/env bash
# Self-test for what mempool_multi_node_bench.sh puts in the metrics history: a
# run that finished contributes a throughput data point, a run that failed
# contributes only its logs, a metrics history that cannot be published fails
# loudly rather than quietly costing the data point, and nothing is published
# twice.
#
# The suite publishes from its EXIT trap, so it publishes for a run that died
# part way through as well as for one that got all the way through. The data
# point is what the combined-metrics job exists to produce and what its trend
# line is read from, and a failed run's is a number from a network that never
# fully came up. The commit does not gate it: combined_metrics.toml sets
# MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS to make one commit of its own at
# the end, and commit_metrics.sh stages every file under [metrics/].
#
# The two settings of that variable are different worlds, and both are covered:
# with commits deferred the job commits later, and with them not deferred --
# e2e_payment_multi_node runs this suite on its own that way -- an uncommitted
# metrics history is discarded with the workspace.
#
# The suite is sourced with MEMPOOL_MULTI_NODE_BENCH_SOURCE_ONLY set, so these
# are the real functions, reached through a real EXIT trap; what they shell out
# to is stubbed.
set -euo pipefail

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
fail=0

work_dir=$(mktemp -d)
trap 'rm -rf "$work_dir"' EXIT

check_eq() {
  local what=$1 want=$2 got=$3
  if [[ $want == "$got" ]]; then
    echo "ok   $what"
  else
    echo "FAIL $what"
    echo "  want: $want"
    echo "  got:  $got"
    fail=1
  fi
}

check_contains() {
  local what=$1 needle=$2 haystack=$3
  if [[ $haystack == *"$needle"* ]]; then
    echo "ok   $what"
  else
    echo "FAIL $what: missing '$needle' in:"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  fi
}

check_lacks() {
  local what=$1 needle=$2 haystack=$3
  if [[ $haystack != *"$needle"* ]]; then
    echo "ok   $what"
  else
    echo "FAIL $what: unexpected '$needle' in:"
    printf '%s\n' "$haystack" | sed 's/^/     | /'
    fail=1
  fi
}

# [get_port] is run at source time to resolve the control port.
stub_build_dir="$work_dir/build"
mkdir -p "$stub_build_dir/hogd/utils"
printf '#!/usr/bin/env bash\necho 11111\n' >"$stub_build_dir/hogd/utils/get_port"
chmod +x "$stub_build_dir/hogd/utils/get_port"

# Runs one case, and prints its output plus a final "EFFECT: exited N".
#
# $1 is the status the run ends with, $2 the control log records it captured
# ("none" for a run that launched nothing), $3 whether to reach the publication
# through the EXIT trap once or call it twice directly, $4 the value of
# MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS ("" to leave it unset, as the
# standalone workflow does), and $5 a helper to make fail ("clone" or "copy").
run_case() {
  local exit_code=$1
  local records=${2:-some}
  local mode=${3:-trap}
  local skip_commit=${4:-0}
  local broken=${5:-}
  local case_script="$work_dir/case.sh"
  local status=0

  cat >"$case_script" <<'CASE'
set -Eeuo pipefail

# Sourcing the suite runs git_info.sh, which reads this when set.
GIT_BRANCH=selftest
export GIT_BRANCH
BUILD_DIR=$STUB_BUILD_DIR
WORK_DIR=$STUB_WORK_DIR
export BUILD_DIR WORK_DIR
MEMPOOL_MULTI_NODE_BENCH_SOURCE_ONLY=1
. "$SUITE" >/dev/null

# What the publication reaches out to. Each says what it did, so the checks are
# about side effects rather than about which branch was taken. The remote log
# copy and the coordinator-output collection are functions in the suite; the
# three scripts are run by path, out of a REPO_ROOT pointed at the stubs.
# $BROKEN names one that fails, so a metrics history that cannot be published
# can be told apart from a run with nothing to publish.
write_coordinator_output() { echo 'EFFECT: collected coordinator output'; }
publish_multi_node_logs() {
  echo 'EFFECT: copied logs'
  [[ $BROKEN != copy ]] || return 1
}

REPO_ROOT=$STUB_WORK_DIR
mkdir -p "$REPO_ROOT/scripts"
stub_script() {
  local name=$1 effect=$2 status=0
  [[ $BROKEN != "$name" ]] || status=1
  printf '#!/usr/bin/env bash\necho "%s"\nexit %s\n' "$effect" "$status" \
    >"$REPO_ROOT/scripts/$name.sh"
  chmod +x "$REPO_ROOT/scripts/$name.sh"
}
stub_script clone_metrics_history 'EFFECT: cloned metrics history'
stub_script log_metric 'EFFECT: wrote summary metric'
stub_script commit_metrics 'EFFECT: committed metrics history'

# The rest of [cleanup] talks to eight benchmark hosts over ssh.
ci_enter_cleanup() { :; }
stop_live_log_streams() { :; }
kill_stale_processes() { :; }

if [[ $RECORDS == none ]]; then
  CONTROL_LOG_RECORDS=()
else
  CONTROL_LOG_RECORDS=('hog11|node1_hog_db|/tmp/a.out|/tmp/a.err')
fi

if [[ -n $SKIP_COMMIT ]]; then
  MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS=$SKIP_COMMIT
else
  unset MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS
fi

case $MODE in
trap)
  # As the suite reaches [cleanup]: from an EXIT trap, with [errexit] in force.
  # Calling it inside a [||] list instead would suppress [errexit] within it and
  # the case could not see a publication failure end the run.
  trap 'cleanup "$?"' EXIT
  exit "$EXIT_CODE"
  ;;
twice)
  # The publication the [cleanup] branch for this status reaches, run twice in
  # one shell: the suite reaches it from the end of a successful run and again
  # from the EXIT trap, and the guard against repeating it lives in variables a
  # subshell would not share.
  if [[ $EXIT_CODE -eq 0 ]]; then
    publish_results
    publish_results
  else
    publish_failed_run_logs
    publish_failed_run_logs
  fi
  ;;
esac
CASE

  STUB_BUILD_DIR="$stub_build_dir" STUB_WORK_DIR="$work_dir" \
    SUITE="$SCRIPT_DIR/mempool_multi_node_bench.sh" \
    EXIT_CODE="$exit_code" RECORDS="$records" MODE="$mode" \
    SKIP_COMMIT="$skip_commit" BROKEN="$broken" \
    bash "$case_script" 2>&1 || status=$?
  echo "EFFECT: exited $status"
}

## A run that got all the way through: its numbers are the point of the job.

out=$(run_case 0)
check_contains 'a passing run copies its logs' 'EFFECT: copied logs' "$out"
check_contains 'a passing run writes the summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_contains 'a passing run commits the metrics history' \
  'EFFECT: committed metrics history' "$out"
check_contains 'a passing run succeeds' 'EFFECT: exited 0' "$out"

## The failure this whole check exists for. In the run that prompted it, node1's
## hog-db died on "bind(10.100.0.111:8083): Address already in use", the suite
## failed a readiness wait ten minutes later, and its throughput -- half of it
## from a node that never came up -- was written to the metrics history and
## pushed by the job's own commit at the end.

out=$(run_case 1)
check_contains 'a failed run still copies its logs' 'EFFECT: copied logs' "$out"
check_lacks 'a failed run writes no summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_contains 'a failed run says why it published no metric' \
  'publishing its logs without a summary metric' "$out"
check_contains 'a failed run names the status it is cleaning up after' \
  'suite failed (exit status 1)' "$out"
check_contains 'a failed run keeps its own status' 'EFFECT: exited 1' "$out"

# ...and the logs have to be committed to survive. e2e_payment_multi_node runs
# this suite on its own with commits not deferred and nothing after it to
# commit, so logs left in the workspace go away with it.
check_contains 'a failed run commits its logs when commits are not deferred' \
  'EFFECT: committed metrics history' "$out"

# A signal-terminated suite is a crash rather than a reported failure, and must
# not publish its numbers either.
out=$(run_case 134)
check_lacks 'a crashed run writes no summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_contains 'a crashed run names the signal' \
  'suite failed (exit status 134 (killed by signal 6: SIGABRT))' "$out"
check_contains 'a crashed run keeps its own status' 'EFFECT: exited 134' "$out"

## With commits deferred -- how combined_metrics.toml runs this suite -- the
## commit belongs to the job, which makes one at the end for every suite.

out=$(run_case 0 some trap 1)
check_contains 'a passing deferred run writes the summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_lacks 'a passing deferred run leaves the commit to the job' \
  'EFFECT: committed metrics history' "$out"
check_contains 'a passing deferred run says the commit is deferred' \
  'skipping metrics history commit' "$out"

out=$(run_case 1 some trap 1)
check_contains 'a failed deferred run still copies its logs' \
  'EFFECT: copied logs' "$out"
check_lacks 'a failed deferred run writes no summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_lacks 'a failed deferred run leaves the commit to the job' \
  'EFFECT: committed metrics history' "$out"

## A metrics history that cannot be published is not a run without a data point.
## Quietly skipping the metric would lose the thing the suite ran for, so the
## clone and the copy failing has to end a passing run rather than be absorbed.

for broken in clone_metrics_history copy; do
  out=$(run_case 0 some trap 0 "$broken")
  check_lacks "a passing run does not skip its metric when $broken fails" \
    'EFFECT: wrote summary metric' "$out"
  check_lacks "a passing run does not commit when $broken fails" \
    'EFFECT: committed metrics history' "$out"
  check_contains "a passing run fails when $broken fails" \
    'EFFECT: exited 1' "$out"
done

# On a run that has already failed the balance is the other way: the status to
# report is the benchmark's, so the publication problem is named rather than
# substituted for it.
out=$(run_case 1 some trap 0 clone_metrics_history)
check_contains 'a failed run reports a publication it could not make' \
  "failed to publish this run's logs to the metrics history" "$out"
check_contains 'a failed run keeps its status over the publication problem' \
  'EFFECT: exited 1' "$out"

# ...including when it is the commit that fails, which is the step that decides
# whether the logs survive at all.
out=$(run_case 1 some trap 0 commit_metrics)
check_contains 'a failed run reports a commit it could not make' \
  "failed to publish this run's logs to the metrics history" "$out"
check_contains 'a failed run keeps its status over a failed commit' \
  'EFFECT: exited 1' "$out"

## Publishing is reached twice for the same run -- from the end of the run and
## from the EXIT trap -- and the slow part of it is a log copy per process
## across eight hosts.

out=$(run_case 0 some twice)
check_eq 'a passing run copies its logs once' 1 \
  "$(grep -c 'EFFECT: copied logs' <<<"$out")"
check_eq 'a passing run writes one summary metric' 1 \
  "$(grep -c 'EFFECT: wrote summary metric' <<<"$out")"
check_eq 'a passing run commits once' 1 \
  "$(grep -c 'EFFECT: committed metrics history' <<<"$out")"

out=$(run_case 1 some twice)
check_eq 'a failed run copies its logs once' 1 \
  "$(grep -c 'EFFECT: copied logs' <<<"$out")"

## A run that launched nothing has no logs to copy, and must not clone the
## metrics history just to leave it empty or commit it just to leave it
## unchanged.

for status in 0 1; do
  out=$(run_case "$status" none)
  check_contains "a run that launched nothing says so (exit $status)" \
    'no control log records captured' "$out"
  check_lacks "a run that launched nothing clones nothing (exit $status)" \
    'EFFECT: cloned metrics history' "$out"
  check_lacks "a run that launched nothing writes no metric (exit $status)" \
    'EFFECT: wrote summary metric' "$out"
  check_lacks "a run that launched nothing commits nothing (exit $status)" \
    'EFFECT: committed metrics history' "$out"
  check_contains "a run that launched nothing keeps its status (exit $status)" \
    "EFFECT: exited $status" "$out"
done

if ((fail)); then
  echo 'multi-node publish selftest FAILED'
  exit 1
fi

echo 'multi-node publish selftest passed'
