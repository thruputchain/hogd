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
  sha256sum release/hogd/hog_exec/bench/payments_bench
  ci_remote_run "$db_host" 'cd /tmp/hogjob-hogci && sha256sum release/hogd/hog_exec/bench/bench_server.base release/hogd/hog_exec/bench/bench_server.pr'
} > "$out/environment.txt"

# ABBA: each invocation recreates the database and event service, then runs
# the original complete two-account workload, including all ten repetitions.
round=0
for variant in base pr pr base; do
  round=$((round + 1))
  label="$round-$variant"
  ci_control_stop_hosts "$control" "$port" "${hosts[@]}"
  ci_remote_run "$db_host" "cd /tmp/hogjob-hogci && ln -sfn bench_server.$variant release/hogd/hog_exec/bench/bench_server"
  printf 'Starting A/B round %s at %s\n' "$label" "$(date -u +%FT%TZ)"
  if ! ./release/hogd/hog_exec/bench/payments_bench -c 10001 -e 10000 -a 2 -s "$db_host" -s "$event_host" > "$out/$label.txt" 2> "$out/$label.log"; then
    tail -100 "$out/$label.log"
    exit 1
  fi
  cat "$out/$label.txt"
done

METRICS_COMMIT_REPO=hogd-item-lock-ab ./scripts/commit_metrics.sh
