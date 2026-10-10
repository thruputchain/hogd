#!/usr/bin/env bash
set -Eeuo pipefail
source ./scripts/ci/bench_harness.sh
control=./release/hogd/hog_exec/bench/control_cli
port=11111
db_host=$HOG_HOST_db
event_host=$HOG_HOST_event
hosts=("$db_host" "$event_host")
cleanup() {
  ci_enter_cleanup
  ci_control_shutdown_hosts "$control" "$port" "${hosts[@]}"
}
trap cleanup EXIT
CI_WAIT_CONTROL_TIMEOUT_SEC=1200 ci_wait_control_server_hosts "$control" "$port" "${hosts[@]}"
./scripts/clone_metrics_history.sh
out="metrics_history/metrics/item-lock-state/$GIT_BRANCH/$GIT_COMMIT"
mkdir -p "$out"
{
  date -u
  uname -a
  lscpu
  sha256sum release/hogd/hog_exec/bench/payments_bench
  ci_remote_run "$db_host" 'cd /tmp/hogjob-hogci && sha256sum release/hogd/hog_exec/bench/bench_server.{base,pr}'
} >"$out/environment.txt"
profile_slow() {
  local client_pid=$1 label=$2
  if python3 scripts/ci/item_lock_watch.py "$client_pid" "$out/$label.log" >"$out/$label-trigger.txt"; then
    bash scripts/ci/item_lock_profile.sh "$client_pid" "ab_profiles/$label-client" "$out/$label.log" &
    local profiler_pid=$!
    ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && bash scripts/ci/item_lock_profile.sh \"\$(cat ab-server.pid)\" ab_profiles/$label-db" || true
    wait "$profiler_pid" || true
    mkdir -p "$out/$label-profile"
    find "ab_profiles/$label-client" -type f ! -name perf.data -exec cp {} "$out/$label-profile/" \;
    ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && tar -czf - --exclude=perf.data ab_profiles/$label-db" >"ab_profiles/$label-db.tar.gz"
    tar -xzf "ab_profiles/$label-db.tar.gz" -C "$out/$label-profile"
  fi
}
round=0
for entry in base:on pr:on pr:on base:on pr:off pr:on pr:off base:on; do
  IFS=: read -r variant recycle <<<"$entry"
  round=$((round + 1))
  label="$round-$variant-recycle-$recycle"
  ci_control_stop_hosts "$control" "$port" "${hosts[@]}"
  ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && echo '$variant $label $recycle' >ab-active.txt"
  echo "Starting $label at $(date -u +%FT%TZ)"
  HOG_LOCK_DIAGNOSTIC=1 ./release/hogd/hog_exec/bench/payments_bench \
    -c 10001 -e 10000 -a 2 -s "$db_host" -s "$event_host" \
    >"$out/$label.txt" 2>"$out/$label.log" &
  client_pid=$!
  profile_slow "$client_pid" "$label" &
  profile_pid=$!
  if ! wait "$client_pid"; then
    tail -100 "$out/$label.log"
    exit 1
  fi
  wait "$profile_pid" || true
  ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && cat ab_profiles/$label-db.log" >"$out/$label-db.log"
  cat "$out/$label.txt"
done
METRICS_COMMIT_REPO=hogd-item-lock-state ./scripts/commit_metrics.sh
