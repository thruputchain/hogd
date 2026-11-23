# Sourced by the stock Combined controller and focused benchmark wrappers.
combined_provenance_selected() {
  [[ ${GIT_BRANCH:-} == codex/combined-transport-provenance ]]
}

combined_provenance_check() {
  [[ ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
    ${HOG_HOST:-} == hog10 && ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
    ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" &&
    ${HOGD_COMBINED_WORK_DIR:-ci_work_dir/} == ci_work_dir/ ]] || {
    echo 'Combined provenance requires the exact offline workspace-storage diagnostic on hog10' >&2
    return 2
  }
}

combined_provenance_peer() {
  local host=$1 command=$2
  if [[ $host == hog10 ]]; then
    timeout --kill-after=2s 30s bash -c "$command"
  else
    timeout --kill-after=2s 30s python3 /opt/hog-ci/peer.py "$host" "$command" </dev/null
  fi
}

combined_provenance_parallel() {
  local action=$1 stage=${2:-} host cmd pid status=0
  local -a pids=()
  shift 2
  for host in "$@"; do
    case $action in
    init)
      printf -v cmd 'python3 -I scripts/ci/combined_transport_provenance.py init %q %q %q' "$host" "$HOG_XID" "$GIT_COMMIT"
      ;;
    start)
      printf -v cmd 'python3 -I scripts/ci/combined_transport_provenance.py sample %q %q %q %q </dev/null >.ci/combined-transport-provenance/%s.stderr 2>&1 & for i in {1..100}; do [ -f .ci/combined-transport-provenance/%s.ready ] && exit 0; sleep .1; done; exit 1' "$host" "$HOG_XID" "$GIT_COMMIT" "$stage" "$stage" "$stage"
      ;;
    stop)
      printf -v cmd 'touch -- .ci/combined-transport-provenance/%s.stop; for i in {1..40}; do tail -n 1 .ci/dumps/combined-transport-provenance/%s.jsonl 2>/dev/null | grep -q "stopped_by_controller.*: true" && exit 0; sleep .1; done; exit 1' "$stage" "$stage"
      ;;
    *) return 2 ;;
    esac
    (
      combined_provenance_peer "$host" "$cmd" || {
        echo "Combined provenance $action failed for $stage on $host" >&2
        exit 1
      }
    ) &
    pids+=("$!")
  done
  for pid in "${pids[@]}"; do
    wait "$pid" || status=1
  done
  return "$status"
}

combined_provenance_run() (
  local stage=$1 rc
  local -a hosts=(hog10 hog11 hog8)
  shift
  combined_provenance_check || exit $?
  case $stage in
  single | multi | fibonacci | payments-2 | payments-10) ;;
  *) exit 2 ;;
  esac
  if [[ $stage == single || $stage == multi ]]; then
    hosts=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
  fi
  if [[ $stage == single ]]; then
    # Stock preceding suites normally start every bench control server. Verify
    # the current build before reading immutable outputs and beginning timing.
    CI_WAIT_CONTROL_TIMEOUT_SEC=600 ci_wait_control_server_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${hosts[@]}" || exit 1
    combined_provenance_parallel init "$stage" "${hosts[@]}" || exit 1
  fi
  # Only exact, own run-local observer files can be affected by stop.
  trap 'rc=$?; trap - EXIT; combined_provenance_parallel stop "$stage" "${hosts[@]}" || { [[ $rc != 0 ]] || rc=1; }; exit "$rc"' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  combined_provenance_parallel start "$stage" "${hosts[@]}" || exit 1
  "$@"
)
