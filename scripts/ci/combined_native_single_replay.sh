# Activated only by the protected Combined driver selecting this exact source branch.
native_single_selected() {
  [[ ${GIT_BRANCH:-} == codex/combined-native-single-replay ||
    ${GIT_BRANCH:-} == codex/combined-native-single-proposal-dag ||
    ${GIT_BRANCH:-} == codex/combined-native-single-post-proposal ]]
}

native_single_check_host() {
  native_single_selected &&
    [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${HOGD_CI_OFFLINE:-0} == 1 &&
      ${HOG_HOST:-} =~ ^hog([4-9]|10|11)$ &&
      ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
      ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" &&
      ${HOGD_COMBINED_WORK_DIR:-ci_work_dir/} == ci_work_dir/ ]]
}

native_single_check() {
  native_single_check_host && [[ $HOG_HOST == hog10 ]]
}

native_single_peer() {
  # The same authenticated run-local adapter used by the f505 peer scripts.
  timeout --kill-after=2s 30s python3 /opt/hog-ci/peer.py "$1" "$2" </dev/null
}

native_single_binary_receipt() (
  set -e -o pipefail
  local protocol=a28763804e9b136891ce6fcfd022e835d7c0cab2
  local protocol_tree=153b761d1e9bee10553f34058b84748f5a0dac0f
  local go_commit=dd4576992574e4e55ab30d3752661ce1c8108a70
  local chuckd=714ff40dcbbcbcb2cb851ce0de1440c37204d595
  local binary sha image
  local -a binaries=(hogd/hog_exec/bench/control_server)
  native_single_check_host
  [[ $(git -C protocol rev-parse HEAD) == "$protocol" &&
  $(git -C protocol rev-parse 'HEAD^{tree}') == "$protocol_tree" &&
  $(git -C thruput-go rev-parse HEAD) == "$go_commit" &&
  $(git -C thruput-go/chuckd rev-parse HEAD) == "$chuckd" &&
  $(findmnt -rn -o TARGET,FSTYPE -T /workspace) == '/workspace ext4' ]]
  grep -Fxq 'CMAKE_BUILD_TYPE:STRING=Release' release/CMakeCache.txt
  image=$(python3 -I -c '
import json,os,re
with open("/workspace/.ci/input-manifest.json") as f:
    doc=json.load(f)
assert doc["sha"] == os.environ["GIT_COMMIT"]
image=doc["image"]
assert re.fullmatch(r"[^\s]+@sha256:[a-f0-9]{64}", image)
print(image)
')
  case $HOG_HOST in
  hog4) binaries+=(chuckd.dev scpd.dev) ;;
  hog5 | hog6) binaries+=(hogd/hog_exec/hog-exec) ;;
  hog7) binaries+=(hogd/loadgen/loadgen_server) ;;
  hog8) binaries+=(hogd/hog_auth/hog-auth) ;;
  hog9) binaries+=(hogd/hog_event/hog-event hogd/hog_indexer/hog-indexer) ;;
  hog10) binaries+=(hogd/hog_mempool/hog-mempool hogd/hog_exec/bench/control_cli) ;;
  hog11) binaries+=(hogd/hog_db/hog-db) ;;
  esac
  (
    set -o noclobber
    for binary in "${binaries[@]}"; do
      [[ -f release/$binary && -x release/$binary && $(realpath -e "release/$binary") == "/workspace/release/$binary" ]]
      readelf -h "release/$binary" >/dev/null
      case $binary in
      hogd/*)
        strings "release/$binary" | grep -Fx "$GIT_COMMIT" >/dev/null
        strings "release/$binary" | grep -Fx "$protocol" >/dev/null
        ;;
      esac
      sha=$(sha256sum "release/$binary")
      printf 'host=%s run=%s path=release/%s source=%s protocol=%s protocol_tree=%s thruput_go=%s chuckd=%s image=%s workspace=ext4 sha256=%s\n' \
        "$HOG_HOST" "$HOG_XID" "$binary" "$GIT_COMMIT" "$protocol" "$protocol_tree" "$go_commit" "$chuckd" "$image" "${sha%% *}"
    done
  ) >"$HOG_DUMP_DIR/native-single-binaries.txt"
)

native_single_observe_roles() {
  local host pid status=0
  local -a pids=()
  for host in "$@"; do
    native_single_peer "$host" 'source /workspace/scripts/ci/combined_native_single_replay.sh; native_single_binary_receipt' &
    pids+=("$!")
  done
  for pid in "${pids[@]}"; do wait "$pid" || status=1; done
  return "$status"
}

native_single_released_hosts=
native_single_shutdown_hosts() {
  local host i status=0
  local -a pids=() hosts=()
  for host in "$@"; do
    [[ $native_single_released_hosts == *":$host:"* ]] && continue
    hosts+=("$host")
    timeout --kill-after=2s 10s "$CONTROL_CLI" "$host" "$CI_CONTROL_PORT" shutdown &
    pids+=("$!")
  done
  for i in "${!pids[@]}"; do
    host=${hosts[$i]}
    if wait "${pids[$i]}"; then
      native_single_released_hosts="$native_single_released_hosts:$host:"
      printf 'Historical single replay: checked shutdown acknowledged for %s\n' "$host"
    else
      printf 'Historical single replay: shutdown failed for %s\n' "$host" >&2
      status=1
    fi
  done
  return "$status"
}

native_single_prepare_post_proposal() {
  [[ $GIT_BRANCH == codex/combined-native-single-post-proposal ]] || return 0
  local command='mkdir -p /workspace/.ci/tmp; set -o noclobber; bash /workspace/scripts/ci/historical_chuckd_post_proposal.sh > /workspace/.ci/dumps/historical-chuckd-build.txt'
  ci_wait_port hog4 18443 &&
    timeout --kill-after=5s 200s python3 /opt/hog-ci/peer.py hog4 "$command" </dev/null
}

native_single_execute() {
  local status=0
  local -a hosts=(hog4 hog5 hog6 hog7 hog8 hog9 hog10 hog11)
  native_single_check || return
  if [[ ${BUILD_DIR:-} != release || ${WORK_DIR:-} != ci_work_dir/ ||
    $# != 1 || $1 != ./scripts/ci/mempool_bench.sh ]]; then
    echo 'Historical single replay needs the stock Combined argv and workspace' >&2
    return 2
  fi
  CI_WAIT_CONTROL_TIMEOUT_SEC=600 ci_wait_control_server_hosts "$CONTROL_CLI" "$CI_CONTROL_PORT" "${hosts[@]}" &&
    native_single_prepare_post_proposal &&
    native_single_observe_roles "${hosts[@]}" &&
    timeout --kill-after=30s 1200s "$@" || status=$?
  native_single_shutdown_hosts "${hosts[@]}" || { [[ $status != 0 ]] || status=1; }
  return "$status"
}
