#!/usr/bin/env bash
# Self-test for what mempool_multi_node_bench.sh puts in the metrics history: a
# run that finished contributes a throughput data point, a run that failed
# contributes only its logs, and neither contributes anything twice.
#
# The suite publishes from its EXIT trap, so it publishes for a run that died
# part way through as well as for one that got all the way through. The data
# point is what the combined-metrics job exists to produce and what its trend
# line is read from, and a failed run's is a number from a network that never
# fully came up. The commit does not gate it: the job sets
# MEMPOOL_MULTI_NODE_BENCH_SKIP_COMMIT_METRICS to make one commit of its own at
# the end, and commit_metrics.sh stages every file under [metrics/].
#
# The suite is sourced with MEMPOOL_MULTI_NODE_BENCH_SOURCE_ONLY set, so these
# are the real functions; what they reach out to -- the metrics-history clone,
# the log copies, log_metric.sh, commit_metrics.sh -- is stubbed.
set -euo pipefail

SCRIPT_DIR=$(dirname "$(readlink -f "$0")")
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
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

# Runs one case. $1 is the exit status the suite is cleaning up after, $2 the
# control log records to have captured ("none" for a run that launched nothing),
# and $3 whether to go through [cleanup] once or the publication it reaches
# twice. Prints the case's output, with a line per side effect the stubs saw.
run_case() {
  local exit_code=$1
  local records=${2:-some}
  local mode=${3:-cleanup}
  local case_script="$work_dir/case.sh"

  cat >"$case_script" <<'CASE'
set -euo pipefail

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
publish_multi_node_logs() { echo 'EFFECT: copied logs'; }
write_coordinator_output() { echo 'EFFECT: collected coordinator output'; }

REPO_ROOT=$STUB_WORK_DIR
mkdir -p "$REPO_ROOT/scripts"
printf '#!/usr/bin/env bash\necho "EFFECT: cloned metrics history"\n' \
  >"$REPO_ROOT/scripts/clone_metrics_history.sh"
printf '#!/usr/bin/env bash\necho "EFFECT: wrote summary metric $1"\n' \
  >"$REPO_ROOT/scripts/log_metric.sh"
printf '#!/usr/bin/env bash\necho "EFFECT: committed metrics history"\n' \
  >"$REPO_ROOT/scripts/commit_metrics.sh"
chmod +x "$REPO_ROOT/scripts"/{clone_metrics_history,log_metric,commit_metrics}.sh

# The rest of [cleanup] talks to eight benchmark hosts over ssh.
ci_enter_cleanup() { :; }
stop_live_log_streams() { :; }
kill_stale_processes() { :; }

if [[ $RECORDS == none ]]; then
  CONTROL_LOG_RECORDS=()
else
  CONTROL_LOG_RECORDS=('hog11|node1_hog_db|/tmp/a.out|/tmp/a.err')
fi

case $MODE in
cleanup)
  # [cleanup] ends in [exit], so run it where that is not the end of the case.
  (cleanup "$EXIT_CODE") || echo "EFFECT: exited $?"
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
    publish_multi_node_diagnostics || true
    publish_multi_node_diagnostics || true
  fi
  ;;
esac
CASE

  STUB_BUILD_DIR="$stub_build_dir" STUB_WORK_DIR="$work_dir" \
    SUITE="$SCRIPT_DIR/mempool_multi_node_bench.sh" \
    EXIT_CODE="$exit_code" RECORDS="$records" MODE="$mode" \
    bash "$case_script" 2>&1
}

## A run that got all the way through: its numbers are the point of the job.

out=$(run_case 0)
check_contains 'a passing run copies its logs' 'EFFECT: copied logs' "$out"
check_contains 'a passing run writes the summary metric' \
  'EFFECT: wrote summary metric nightly_integration_mempool_multi_node' "$out"
check_contains 'a passing run commits the metrics history' \
  'EFFECT: committed metrics history' "$out"

## The failure this whole check exists for. In the run that prompted it, node1's
## hog-db died on "bind(10.100.0.111:8083): Address already in use", the suite
## failed a readiness wait ten minutes later, and its throughput -- half of it
## from a node that never came up -- was written to the metrics history and
## pushed by the job's own commit at the end.

out=$(run_case 1)
check_contains 'a failed run still copies its logs' 'EFFECT: copied logs' "$out"
check_lacks 'a failed run writes no summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_lacks 'a failed run commits nothing' \
  'EFFECT: committed metrics history' "$out"
check_contains 'a failed run says why it published no metric' \
  'publishing its logs without a summary metric' "$out"
check_contains 'a failed run names the status it is cleaning up after' \
  'suite failed (exit status 1)' "$out"

# A signal-terminated suite is a crash rather than a reported failure, and must
# not publish either.
out=$(run_case 134)
check_lacks 'a crashed run writes no summary metric' \
  'EFFECT: wrote summary metric' "$out"
check_contains 'a crashed run names the signal' \
  'suite failed (exit status 134 (killed by signal 6: SIGABRT))' "$out"

## Publishing is reached twice for the same run -- from the end of the run and
## from the EXIT trap -- and the slow part of it is a log copy per process across
## eight hosts.

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
## metrics history just to leave it empty.

out=$(run_case 1 none)
check_contains 'a run that launched nothing says so' \
  'no control log records captured' "$out"
check_lacks 'a run that launched nothing clones nothing' \
  'EFFECT: cloned metrics history' "$out"
check_lacks 'a run that launched nothing writes no metric' \
  'EFFECT: wrote summary metric' "$out"

out=$(run_case 0 none)
check_lacks 'a passing run that launched nothing writes no metric' \
  'EFFECT: wrote summary metric' "$out"
check_lacks 'a passing run that launched nothing commits nothing' \
  'EFFECT: committed metrics history' "$out"

if ((fail)); then
  echo 'multi-node publish selftest FAILED'
  exit 1
fi

echo 'multi-node publish selftest passed'
