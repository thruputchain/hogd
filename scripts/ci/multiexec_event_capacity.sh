#!/usr/bin/env bash
set -Eeuo pipefail

[[ ${GIT_BRANCH:-} == codex/multiexec-event-capacity &&
   ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 && ${HOG_HOST:-} == hog10 &&
   ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
   ${HOGD_EVENT_WORK_DIR:-} == /bench-event/ci_work_dir &&
   ${HOG_DUMP_DIR:-} == /workspace/.ci/dumps &&
   ${GIT_COMMIT:-} =~ ^[0-9a-f]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" &&
   -r /opt/hog-ci/peer.py && $# == 1 && $1 == release ]]
runner="./$1/hogd/hog_exec/bench/multiexec_runner"
control="./$1/hogd/hog_exec/bench/control_cli"
[[ -x $runner && -x $control && ${CI_CONTROL_PORT:-} =~ ^[0-9]+$ ]]
helper=scripts/ci/multiexec_event_capacity.py
out=$(mktemp -d -- "$HOG_DUMP_DIR/multiexec-event-capacity.XXXXXXXX")
printf 'host=hog11\ndriver=hog10\nsource=%s\nrun=%s\n' "$GIT_COMMIT" "$HOG_XID" >"$out/provenance.txt"
sha256sum "$runner" "$control" "$helper" >"$out/inputs.sha256"

peer() {
  timeout --kill-after=2s "$1" python3 /opt/hog-ci/peer.py hog11 "$2" </dev/null
}

run_case() (
  executors=$1 batch=$2
  shift 2
  arm="$out/$executors-exec-batch-$batch"
  mkdir -m 700 -- "$arm"
  directory=
  runner_pid=
  sampler_pid=

  cleanup() {
    status=$?
    trap - EXIT
    trap '' INT TERM
    if [[ -n $runner_pid ]]; then
      kill -TERM "$runner_pid" 2>/dev/null || true
      for ((attempt=0; attempt<50; attempt++)); do
        kill -0 "$runner_pid" 2>/dev/null || break
        sleep .1
      done
      if kill -0 "$runner_pid" 2>/dev/null; then
        kill -KILL "$runner_pid" 2>/dev/null || true
      fi
      wait "$runner_pid" 2>/dev/null || true
    fi
    if [[ -n $directory ]]; then
      if ! timeout --kill-after=2s 30s "$control" hog11 "$CI_CONTROL_PORT" stop_checked         >"$arm/stop.log" 2>&1; then
        echo "Retaining Event data after checked stop failed: $directory" >&2
        exit 1
      fi
      if [[ -n $sampler_pid ]]; then
        if ! peer 15s "test -O $directory; test ! -L $directory; touch -- $directory/stop"; then
          echo "Could not stop capacity sampler: $directory" >&2
          exit 1
        fi
        wait "$sampler_pid" || status=1
      fi
      if ! peer 30s "python3 /workspace/$helper reclaim $directory" >"$arm/cleanup.json"; then
        status=1
      fi
    fi
    if (( status == 0 )); then
      python3 "$helper" validate "$arm" "$executors" "$batch" || status=1
    fi
    exit "$status"
  }
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  allocated=$(peer 15s "python3 /workspace/$helper allocate")
  [[ $allocated =~ ^/bench-event/multiexec-capacity\.[a-zA-Z0-9]{8}$ ]]
  directory=$allocated
  printf '%s\n' "$directory" >"$arm/path.txt"
  printf '%s\n' "$@" >"$arm/roles.txt"
  peer 330s "python3 /workspace/$helper sample $directory" >"$arm/samples.jsonl" 2>"$arm/sampler.stderr" &
  sampler_pid=$!
  for ((attempt=0; attempt<100; attempt++)); do
    [[ -s $arm/samples.jsonl ]] && break
    kill -0 "$sampler_pid" 2>/dev/null || exit 1
    sleep .1
  done
  [[ -s $arm/samples.jsonl ]]
  timeout --kill-after=3s 300s "$runner" "$@" -a 100 -b "$batch" -E "$directory/event" \
    >"$arm/runner.log" 2>"$arm/runner.stderr" &
  runner_pid=$!
  status=0
  wait "$runner_pid" || status=$?
  runner_pid=
  exit "$status"
)

# Keep stock position/order, swapping hog7 and hog11 to use the already
# provisioned Event tmpfs. DB/indexer and all other executor slots stay fixed.
for executors in 1 2 3; do
  args=(-s hog11 -s hog8 -s hog4 -s hog5)
  if (( executors >= 2 )); then args+=(-s hog7); fi
  if (( executors == 3 )); then args+=(-s hog6); fi
  for batch in 1 5 10; do
    run_case "$executors" "$batch" "${args[@]}"
  done
done
python3 "$helper" summarize "$out" >"$out/summary.json"
