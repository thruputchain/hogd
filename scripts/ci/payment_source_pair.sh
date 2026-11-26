#!/usr/bin/env bash
# Called only after stock prepayment metrics in the protected standalone.
set -Eeuo pipefail
[[ $# == 4 && ${GIT_BRANCH:-} == codex/payment-sparse-phase-screen &&
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
# Read only immutable linked libraries for these known versioned binaries.
# Receipt work is outside every stock benchmark invocation.
ldd "./$build/hogd/hog_exec/bench/payments_bench" >"$evidence/driver.ldd.txt"
ci_remote_run hog11 'ldd /workspace/release/hogd/hog_exec/bench/bench_server' >"$evidence/db.ldd.txt"
ci_remote_run hog8 'ldd /workspace/release/hogd/hog_event/hog-event' >"$evidence/event.ldd.txt"
for probe in off on; do
  arm="$evidence/$probe"
  mkdir -m 700 -- "$arm"
  for acct in 10 2; do
    receipt="$arm/$acct.receipts"
    ci_remote_run hog11 "bash /workspace/scripts/ci/payment_source_role_receipt.sh db before$acct" >"$receipt"
    ci_remote_run hog8 "bash /workspace/scripts/ci/payment_source_role_receipt.sh event before$acct" >>"$receipt"
    if [[ $probe == off ]]; then
      ./scripts/make_metric_file.sh "payments-bench-rv32-$acct" "$branch" "$commit" "$time"
      output="metrics_history/metrics/payments-bench-rv32-$acct/$branch/${time}_$commit"
      cmd=(env -u HOGD_PAYMENT_PHASE_FILE ./scripts/metrics/payments_bench.sh "$build" "$acct")
    else
      output="$arm/$acct.stdout"
      cmd=(env HOGD_PAYMENT_PHASE_FILE="$arm/$acct.phase.tsv" ./scripts/metrics/payments_bench.sh "$build" "$acct")
    fi
    date +%s.%N >"$arm/$acct.start"
    status=0
    "${cmd[@]}" >"$output" || status=$?
    date +%s.%N >"$arm/$acct.end"
    printf '%s\n' "$status" >"$arm/$acct.status"
    ci_control_stop_hosts "$control" "$port" hog8 hog11
    ci_remote_run hog11 "bash /workspace/scripts/ci/payment_source_role_receipt.sh db after$acct" >>"$receipt"
    ci_remote_run hog8 "bash /workspace/scripts/ci/payment_source_role_receipt.sh event after$acct" >>"$receipt"
    sha256sum -c "$evidence/client.sha256"
    [[ $status == 0 ]]
    if [[ $probe == off ]]; then
      cp -- "$output" "$arm/$acct.stdout"
      [[ ! -e $arm/$acct.phase.tsv ]]
    else
      [[ -s $arm/$acct.phase.tsv ]]
      [[ $(head -n 1 "$arm/$acct.phase.tsv") == PAYMENT_SPARSE_V1 ]]
    fi
  done
done
