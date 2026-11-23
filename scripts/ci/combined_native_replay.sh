# Sourced by the historical Combined drivers. A separate branch labels the
# intentionally partial diagnostic without touching regular benchmark runs.
combined_native_replay_selected() {
  [[ ${GIT_BRANCH:-} == codex/combined-native-rpc-replay && ${HOGD_CI_OFFLINE:-0} == 1 ]]
}

combined_native_replay_check() {
  if [[ ${HOGD_CI_CONTAINER:-0} != 1 || ${HOG_HOST:-} != hog10 ||
    ! ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ ||
    ! ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ || $(git rev-parse HEAD) != "$GIT_COMMIT" ]]; then
    echo 'Combined native-source replay requires a protected Combined container run on hog10' >&2
    return 2
  fi
}

# This state belongs only to the current controller shell.
combined_native_replay_released_hosts=
# Only an acknowledged checked shutdown permits skipping future commands.
combined_native_replay_released() {
  [[ ${combined_native_replay_released_hosts:-} == *":$1:"* ]]
}

# Collect once after the normal current-build readiness handshake, before the
# timed scripts. Runtime clients also announce their own SHA256 prefixes.
combined_native_replay_binary_receipts() (
  set -e -o pipefail
  local protocol=a28763804e9b136891ce6fcfd022e835d7c0cab2
  local protocol_tree=153b761d1e9bee10553f34058b84748f5a0dac0f
  local binary sha
  local -a binaries
  combined_native_replay_selected
  [[ ${HOGD_CI_CONTAINER:-0} == 1 && ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
    ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" &&
    $(git -C protocol rev-parse HEAD) == "$protocol" &&
    $(git -C protocol rev-parse 'HEAD^{tree}') == "$protocol_tree" ]]
  case ${HOG_HOST:-} in
  hog10) binaries=(hogd/hog_exec/bench/fibonacci_rv32only hogd/hog_exec/bench/payments_bench) ;;
  hog11) binaries=(hogd/hog_exec/bench/bench_server) ;;
  hog8) binaries=(hogd/hog_event/hog-event) ;;
  *) return 2 ;;
  esac
  for binary in "${binaries[@]}"; do
    [[ -f release/$binary && -x release/$binary && ! -L release/$binary ]]
    strings "release/$binary" | grep -Fx "$GIT_COMMIT" >/dev/null
    strings "release/$binary" | grep -Fx "$protocol" >/dev/null
    sha=$(sha256sum "release/$binary")
    printf 'HISTORICAL_RPC_BINARY host=%s path=release/%s source=%s protocol=%s tree=%s sha256=%s\n' \
      "$HOG_HOST" "$binary" "$GIT_COMMIT" "$protocol" "$protocol_tree" "${sha%% *}"
  done
)
