# Sourced by the historical Combined drivers. A separate branch labels the
# intentionally partial diagnostic without touching regular benchmark runs.
combined_native_replay_selected() {
  [[ ${GIT_BRANCH:-} == codex/combined-native-source-replay && ${HOGD_CI_OFFLINE:-0} == 1 ]]
}

combined_native_replay_check() {
  if [[ ${HOGD_CI_CONTAINER:-0} != 1 || ${HOG_HOST:-} != hog10 ||
    ! ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ ]]; then
    echo 'Combined native-source replay requires a protected Combined container run on hog10' >&2
    return 2
  fi
}
