#!/usr/bin/env bash
# Same driver/DB/Event binaries, full stock 10-account payments geometry; only
# Event's private media varies. All four arms retained, never publish.
set -Eeuo pipefail
[[ $# == 2 && ${GIT_BRANCH:-} == codex/payment-event-media-abba &&
   ${HOG_HOST:-} == hog10 && ${HOGD_CI_CONTAINER:-0} == 1 &&
   ${HOGD_CI_OFFLINE:-0} == 1 && ${HOGD_COMBINED_WORK_DIR:-} == /bench-combined/ci_work_dir/ &&
   ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ ]]
build=$1 output=$2
source "$(dirname "$0")/bench_harness.sh"
control="./$build/hogd/hog_exec/bench/control_cli"
port=$("./$build/hogd/utils/get_port" kCIControlServerPort)
evidence="$HOG_DUMP_DIR/payment-event-media-abba"
mkdir -m 700 -- "$evidence"
cp /proc/self/status "$evidence/driver-status.txt"
arm= sampler= prepared=no
host_action() { ci_remote_run "$1" "bash /workspace/scripts/ci/payment_event_media_host.sh $2 $arm"; }
cleanup_arm() {
  local status=0
  ci_control_stop_hosts "$control" "$port" hog8 hog11
  host_action hog11 db-receipt >>"$evidence/$arm.receipts" || status=1
  if [[ $prepared == yes ]]; then
    host_action hog8 reclaim >>"$evidence/$arm.receipts" || status=1
  fi
  if [[ -n $sampler ]]; then
    if ((status)); then kill "$sampler" 2>/dev/null || true; fi
    wait "$sampler" || status=1
  fi
  sampler=; prepared=no
  return "$status"
}
finish() { local status=$?; trap - EXIT; if [[ -n $arm ]]; then cleanup_arm || status=1; fi; exit "$status"; }
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
for slot in 1-disk 2-ram 3-ram 4-disk; do
  arm=$slot
  host_action hog11 db-receipt >"$evidence/$arm.receipts"
  host_action hog8 prepare >>"$evidence/$arm.receipts"
  prepared=yes
  host_action hog8 sample </dev/null >"$evidence/$arm.sampler.log" 2>&1 & sampler=$!
  host_action hog8 sample-ready
  date +%s.%N >"$evidence/$arm.start"
  status=0
  HOGD_PAYMENT_EVENT_MEDIA_ARM="$arm" timeout -k 10 600 ./scripts/metrics/payments_bench.sh "$build" 10 >"$evidence/$arm.stdout" || status=$?
  date +%s.%N >"$evidence/$arm.end"
  printf '%s\n' "$status" >"$evidence/$arm.status"
  cleanup_arm
  arm=
  [[ $status == 0 ]]
  if [[ $slot == 1-disk ]]; then cp -- "$evidence/$slot.stdout" "$output"; fi
done
