#!/usr/bin/env bash
set -Eeuo pipefail

# The Combined Metrics driver is the only job with this delegated Event path.
[[ ${GIT_BRANCH:-} == codex/multiexec-event-storage &&
   ${HOGD_CI_CONTAINER:-} == 1 && ${HOG_HOST:-} == hog10 &&
   ${HOGD_EVENT_WORK_DIR:-} == /bench-event/ci_work_dir &&
   ${HOG_DUMP_DIR:-} == /workspace/.ci/dumps &&
   $(command -v ssh) == /opt/hog-ci/bin/ssh ]]

. "$(dirname "$0")/bench_harness.sh"

runner="./${BUILD_DIR:?}/hogd/hog_exec/bench/multiexec_runner"
control="./$BUILD_DIR/hogd/hog_exec/bench/control_cli"
event_host=hog11
output=$(mktemp -d "$HOG_DUMP_DIR/multiexec-event-storage.XXXXXXXXXXXX")
active_dir=

remove_active_arm() {
  if [[ -n $active_dir ]]; then
    [[ $active_dir =~ ^(/workspace|/bench-event)/multiexec-event-storage\.[[:alnum:]]{12}$ ]] ||
      return 1
    ci_remote_run "$event_host" "rm -rf --one-file-system -- $(printf '%q' "$active_dir")" ||
      return 1
    active_dir=
  fi
}

cleanup_storage_experiment() {
  if [[ -n $active_dir ]]; then
    # The control RPC waits for the run's Event process to exit before removal.
    if timeout 30 "$control" "$event_host" "${CI_CONTROL_PORT:?}" stop; then
      remove_active_arm || echo "Could not clean up the Event arm directory" >&2
    else
      echo "Leaving the Event arm directory for container cleanup" >&2
    fi
  fi
}

trap cleanup_storage_experiment EXIT
trap 'exit 130' INT TERM

capture_event_phases=$(cat <<'REMOTE'
dir=$1
for log in /tmp/hogd_stderr_*; do
  if [[ -f $log && ! -L $log ]] && grep -qF -- "$dir" "$log"; then
    grep -E 'block committed height=|slow block phase |PORT event\.' "$log" || :
  fi
done
REMOTE
)

expected_sha=
i=0
for backing in disk tmpfs tmpfs disk; do
  i=$((i + 1))
  arm="$output/$i-$backing"
  mkdir "$arm"
  if [[ $backing == tmpfs ]]; then
    root=/bench-event
    verify='[[ $(stat -f -c %T "$root/ci_work_dir") == tmpfs ]]'
  else
    root=/workspace
    verify='[[ $(stat -f -c %T "$root/ci_work_dir") != tmpfs &&
                $(stat -c %d "$root") == "$(stat -c %d "$root/ci_work_dir")" ]]'
  fi
  active_dir=$(ci_remote_run "$event_host" "set -eu; root=$root; $verify; mktemp -d \"\$root/multiexec-event-storage.XXXXXXXXXXXX\"")
  [[ $active_dir == "$root"/multiexec-event-storage.* &&
     $active_dir =~ ^(/workspace|/bench-event)/multiexec-event-storage\.[[:alnum:]]{12}$ ]]
  ci_remote_run "$event_host" "stat -f -c 'storage=%T fsid=%i' -- $(printf '%q' "$active_dir")" >"$arm/storage.txt"
  if grep -q '^storage=tmpfs ' "$arm/storage.txt"; then
    actual=tmpfs
  else
    actual=disk
  fi
  [[ $actual == "$backing" ]]
  printf 'arm=%s\nbacking=%s\nwork_dir=%s\nhogd_sha=%s\n' \
    "$i" "$backing" "$active_dir" "${GIT_COMMIT:-unknown}" >>"$arm/storage.txt"

  echo "Run Event storage arm $i ($backing)"
  HOG_DUMP_DIR="$arm" HOGD_MULTIEXEC_EVENT_ARM_DIR="$active_dir" \
    "$runner" -s hog11 -s hog8 -s hog4 -s hog5 -s hog7 -s hog6 -a 100 \
    >"$arm/driver.log" 2>&1
  if ! ci_remote_run "$event_host" "bash -c $(printf '%q' "$capture_event_phases") bash $(printf '%q' "$active_dir")" \
      >"$arm/event-phases.log"; then
    echo 'Event phase log unavailable; driver callback diagnostics were retained' >&2
  fi
  if ! grep -q 'block committed height=' "$arm/event-phases.log"; then
    echo "No Event commit phases captured for arm $i; driver callback diagnostics were retained" >&2
  fi
  diagnostics="$arm/multiexec-close-3exec-batch10.log"
  [[ $(grep -c '^multiexec_close:' "$diagnostics") == 7 &&
     $(grep -c '^multiexec_close_exec:' "$diagnostics") == 210 ]]
  arm_sha=$(sed -nE 's/^multiexec_storage_workload_sha256=([[:xdigit:]]{64})$/\1/p' "$arm/driver.log")
  [[ $arm_sha =~ ^[[:xdigit:]]{64}$ ]]
  expected_sha=${expected_sha:-$arm_sha}
  printf 'workload_sha256=%s\n' "$arm_sha" >>"$arm/storage.txt"
  if [[ $arm_sha != "$expected_sha" ]]; then
    echo "Workload fingerprint changed in Event storage arm $i" >&2
    exit 1
  fi
  remove_active_arm
done
