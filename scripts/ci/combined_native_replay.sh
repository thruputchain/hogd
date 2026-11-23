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
