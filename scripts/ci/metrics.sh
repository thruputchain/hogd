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

if [[ ${GIT_BRANCH:-} == codex/payments-db-service-diagnostics &&
      ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
      ${HOG_HOST:-} == hog10 ]]; then
  output="${HOG_DUMP_DIR:?}/payment-phases"
  mkdir -p "$output"
  for accounts in 10 2; do
    expected_workload=
    for arm in llc-a-1 split-1 split-2 llc-b-1 stock; do
      raw="$output/payments-$accounts-$arm.raw"
      (
        set -o noclobber
        "$BUILD_DIR/hogd/hog_exec/bench/payments_bench" -c 10001 -e 10000 \
          -a "$accounts" -s hog11 -s hog8 -A "$arm" >"$raw"
      )
      workload=$(sed -n 's/^payment_db_workload //p' "$raw")
      if [[ ! $workload =~ ^56013\ [1-9][0-9]*\ 2048000\ [0-9a-f]{64}$ ||
            ( -n $expected_workload && $workload != "$expected_workload" ) ]]; then
        echo "Payment workload mismatch for $accounts accounts in $arm" >&2
        exit 1
      fi
      expected_workload=$workload
      printf '%s\n' "$workload" >"$output/payments-$accounts-$arm.workload"
      # TSV columns: accounts, repetition, phase, sample count, p50 ns, p90 ns.
      awk -v phases="$output/payments-$accounts-$arm.tsv" \
          -v elapsed="$output/payments-$accounts-$arm.elapsed" \
          -v callers="$output/payments-$accounts-$arm.db-caller.tsv" \
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
      ' "$raw" || { echo "Incomplete payment phase results for $accounts accounts" >&2; exit 1; }
      rm -- "$raw"
    done
  done
  exit 0
fi

echo 'clone metrics history' # This is idempotent
./scripts/clone_metrics_history.sh

echo 'run metrics'
./scripts/metrics.sh ${BUILD_DIR}
