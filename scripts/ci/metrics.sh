#!/usr/bin/env bash
set -Eeuo pipefail

. "$(dirname "$0")/bench_harness.sh"
ci_enable_error_trace

echo 'set port env vars'
export CI_CONTROL_PORT=$(./${BUILD_DIR}/hogd/utils/get_port kCIControlServerPort)
CONTROL_CLI="./${BUILD_DIR}/hogd/hog_exec/bench/control_cli"
BENCH_HOSTS=(hog11 hog8)

cleanup() {
  local exit_code=$1

  ci_enter_cleanup
  echo 'stop any running processes'
  ci_control_stop_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${BENCH_HOSTS[@]}"

  exit "$exit_code"
}

trap 'cleanup "$?"' EXIT
trap 'cleanup 130' INT TERM

echo 'make sure bench servers are running'
ci_wait_control_server_hosts "$CONTROL_CLI" "${CI_CONTROL_PORT}" \
  "${BENCH_HOSTS[@]}"

queue_pair=0
case ${GIT_BRANCH:-} in
codex/payment-srpc-queue-phases | codex/payment-srpc-queue-control | \
  codex/payment-rpc-frame-phases | codex/payment-rpc-frame-control)
  [[ ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
    ${HOG_HOST:-} == hog10 && ${HOG_XID:-} =~ ^hogd-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
    ${GIT_COMMIT:-} =~ ^[0-9a-f]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" ]]
  queue_pair=1
  ;;
esac

if [[ (${GIT_BRANCH:-} == codex/payments-db-service-diagnostics || $queue_pair == 1) &&
  ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
  ${HOG_HOST:-} == hog10 ]]; then
  output="${HOG_DUMP_DIR:?}/payment-phases"
  mkdir -p "$output"
  if [[ $queue_pair == 1 ]]; then
    printf '%s\t%s\t%s\n' "$GIT_COMMIT" "$GIT_BRANCH" "$HOG_XID" >"$output/queue-provenance.tsv"
  fi
  for accounts in 10 2; do
    raw="$output/payments-$accounts.raw"
    ./scripts/metrics/payments_bench.sh "$BUILD_DIR" "$accounts" >"$raw"
    if [[ $queue_pair == 1 ]]; then
      grep -Ex 'payment_db_workload 56013 [1-9][0-9]* 2048000 [0-9a-f]{64}' \
        "$raw" >"$output/payments-$accounts.workload"
      [[ $(wc -l <"$output/payments-$accounts.workload") == 1 ]]
    fi
    # TSV columns: accounts, repetition, phase, sample count, p50 ns, p90 ns.
    awk -v phases="$output/payments-$accounts.tsv" \
      -v elapsed="$output/payments-$accounts.elapsed" \
      -v callers="$output/payments-$accounts.db-caller.tsv" \
      -v accounts="$accounts" '
      /^payment_phase / {
        if (NF != 7 || $2 != accounts || $3 !~ /^[0-9]$/ ||
            $4 !~ /^[1-6]$/ || $5 != ($4 == 3 ? 1 : 391) ||
            $6 !~ /^[0-9]+$/ || $7 !~ /^[0-9]+$/ || $7 < $6 ||
            seen[$3, $4]++) {
          invalid = 1
        }
        print $2 "\t" $3 "\t" $4 "\t" $5 "\t" $6 "\t" $7 > phases
        rows++
        next
      }
      /^payment_db_caller / {
        if (NF != 11 || $2 != accounts || $3 !~ /^[0-9]$/ ||
            $4 !~ /^[0-9]+$/ || $5 !~ /^[0-9]+$/ || $5 % 256 ||
            $6 !~ /^[0-9]+$/ || $7 !~ /^[0-9]+$/ ||
            $8 !~ /^[0-9]+$/ || $9 !~ /^[0-9]+$/ ||
            $10 !~ /^[0-9]+$/ || $11 !~ /^[0-9]+$/ ||
            $8 + $9 > $6 || $10 + $11 > $7 || caller_seen[$4, $5]++) {
          invalid = 1
        }
        print $2 "\t" $3 "\t" $4 "\t" $5 "\t" $6 "\t" $7 "\t" $8 "\t" $9 "\t" $10 "\t" $11 > callers
        caller_reps[$3]++
        next
      }
      /^time taken: / {
        print > elapsed
        elapsed_rows++
        if ($0 ~ ("^time taken: [0-9]+ us for 100000 txs [(]accts " accounts "[)] on 512 coros$")) {
          matched++
        }
      }
      END {
        if (invalid || rows != 60 || matched != 10 || elapsed_rows != 180) exit 1
        for (i = 0; i < 10; i++) {
          if (caller_reps[i] != 390 && caller_reps[i] != 391) exit 1
        }
      }
    ' "$raw" || {
      echo "Incomplete payment phase results for $accounts accounts" >&2
      exit 1
    }
    rm -- "$raw"
  done
  exit 0
fi

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'run metrics'
./scripts/metrics.sh ${BUILD_DIR}
