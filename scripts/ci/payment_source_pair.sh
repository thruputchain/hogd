#!/usr/bin/env bash
# Called only after the stock pre-payment metrics in standalone Metrics.
set -Eeuo pipefail
[[ $# == 4 && ( ${GIT_BRANCH:-} == codex/payment-source-historical || ${GIT_BRANCH:-} == codex/payment-source-current ) &&
   ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 && ${HOG_HOST:-} == hog10 &&
   ${HOG_XID:-} =~ ^hogd-metrics-[1-9][0-9]*-[1-9][0-9]*$ && ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ ]]
build=$1 branch=$2 commit=$3 time=$4
[[ $build == release && $branch == "$GIT_BRANCH" && $commit == "$GIT_COMMIT" && $(git rev-parse HEAD) == "$GIT_COMMIT" ]]
source "$(dirname "$0")/bench_harness.sh"
control="./$build/hogd/hog_exec/bench/control_cli"
port=$("./$build/hogd/utils/get_port" kCIControlServerPort)
[[ $port == 11111 ]]
evidence="$HOG_DUMP_DIR/payment-source-pair"
mkdir -m 700 -- "$evidence"
[[ $(findmnt -rn -o FSTYPE -T /workspace) == ext4 ]]
cp /proc/self/status "$evidence/driver-status.txt"
findmnt -T /workspace >"$evidence/driver-mount.txt"
findmnt -T /tmp >"$evidence/driver-tmp.txt"
sha256sum "./$build/hogd/hog_exec/bench/payments_bench" >"$evidence/client.sha256"
git submodule status --recursive >"$evidence/submodules.txt"
for acct in 10 2; do
  receipt="$evidence/$acct.receipts"
  ci_remote_run hog11 "bash /workspace/scripts/ci/payment_source_role_receipt.sh db before$acct" >"$receipt"
  ci_remote_run hog8 "bash /workspace/scripts/ci/payment_source_role_receipt.sh event before$acct" >>"$receipt"
  ./scripts/make_metric_file.sh "payments-bench-rv32-$acct" "$branch" "$commit" "$time"
  output="metrics_history/metrics/payments-bench-rv32-$acct/$branch/${time}_$commit"
  date +%s.%N >"$evidence/$acct.start"
  status=0
  ./scripts/metrics/payments_bench.sh "$build" "$acct" >"$output" || status=$?
  date +%s.%N >"$evidence/$acct.end"
  printf '%s\n' "$status" >"$evidence/$acct.status"
  ci_control_stop_hosts "$control" "$port" hog8 hog11
  ci_remote_run hog11 "bash /workspace/scripts/ci/payment_source_role_receipt.sh db after$acct" >>"$receipt"
  ci_remote_run hog8 "bash /workspace/scripts/ci/payment_source_role_receipt.sh event after$acct" >>"$receipt"
  sha256sum -c "$evidence/client.sha256"
  [[ $status == 0 ]]
done
