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
out="metrics_history/metrics/item-lock-ab/$GIT_BRANCH/$GIT_COMMIT"
mkdir -p "$out"
{
  date -u
  uname -a
  lscpu
  printf 'PR source: b0d029d74cc837511b2a2c42f94a97a6cd9cc9ad\nParent source: 9656117d1cfeba2ea5ca61c0a5995f0da2126aec\n'
  printf 'Client: %s; DB: %s; event: %s\n' "$HOSTNAME" "$db_host" "$event_host"
  sha256sum release/hogd/hog_exec/bench/payments_bench.{base,pr}
  if cmp -s release/hogd/hog_exec/bench/payments_bench.base release/hogd/hog_exec/bench/payments_bench.pr; then
    echo "Parent and PR clients are byte-identical"
  else
    echo "Parent and PR clients differ"
  fi
  ci_remote_run "$db_host" 'cd /tmp/hogjob-hogci && sha256sum release/hogd/hog_exec/bench/bench_server.base release/hogd/hog_exec/bench/bench_server.pr'
} >"$out/environment.txt"

# Original, uninstrumented client. First ABBA is unprofiled; the last pair
# collects flat CPU samples on both client and DB during the affected phase.
round=0
profile_phase() {
  local client_pid=$1 label=$2
  while kill -0 "$client_pid" 2>/dev/null; do
    if grep -q 'bench rep 0 for 100000 txs (accts 2) on 1024 threads' "$out/$label.log"; then
      bash scripts/ci/item_lock_profile.sh "$client_pid" "ab_profiles/$label-client" &
      local profiler_pid=$!
      ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && bash scripts/ci/item_lock_profile.sh \"\$(pgrep -n -x bench_server)\" ab_profiles/$label-db" || true
      wait "$profiler_pid" || true
      mkdir -p "$out/$label-profile"
      find "ab_profiles/$label-client" -type f ! -name perf.data -exec cp {} "$out/$label-profile/" \;
      ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && for f in ab_profiles/$label-db/*.txt; do echo \"\$f\"; cat \"\$f\"; done" >"$out/$label-profile/db.txt" || true
      return
    fi
    sleep 0.2
  done
}

entries=(pr:base:no pr:pr:no pr:pr:no pr:base:no)
if ! cmp -s release/hogd/hog_exec/bench/payments_bench.base release/hogd/hog_exec/bench/payments_bench.pr; then
  entries+=(base:base:no base:pr:no)
fi
entries+=(pr:base:yes pr:pr:yes)
for entry in "${entries[@]}"; do
  IFS=: read -r client_variant variant profile <<<"$entry"
  round=$((round + 1))
  label="$round-client-$client_variant-db-$variant-profile-$profile"
  ci_control_stop_hosts "$control" "$port" "${hosts[@]}"
  ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && ln -sfn bench_server.$variant release/hogd/hog_exec/bench/bench_server"
  printf 'Starting A/B round %s at %s\n' "$label" "$(date -u +%FT%TZ)"
  ./release/hogd/hog_exec/bench/payments_bench.$client_variant -c 10001 -e 10000 -a 2 -s "$db_host" -s "$event_host" >"$out/$label.txt" 2>"$out/$label.log" &
  client_pid=$!
  if [[ $profile == yes ]]; then
    profile_phase "$client_pid" "$label" &
    profile_pid=$!
  fi
  if ! wait "$client_pid"; then
    tail -100 "$out/$label.log"
    exit 1
  fi
  if [[ $profile == yes ]]; then
    wait "$profile_pid"
  fi
  cat "$out/$label.txt"
done

METRICS_COMMIT_REPO=hogd-item-lock-ab ./scripts/commit_metrics.sh
