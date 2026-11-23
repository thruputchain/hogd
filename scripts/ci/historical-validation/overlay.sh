# Sourced by the stock offline environment hook.

historical_validation_verify_sources() {
  local original=/workspace/thruput-go/chuckd/sub/baton/baton/vproposal.go
  local replacement=/workspace/scripts/ci/historical-validation/vproposal.go
  [[ $(realpath -e "$original") == "$original" &&
     $(realpath -e "$replacement") == "$replacement" ]] || return 2
  printf '%s  %s\n' \
    51fd85a22877ec83933a9365c4919bf5768a2561d6b2d691a799e08a2ae39063 "$original" \
    c6fc3e434351dc067d9e7711733608b38f44b6d3ce9f3b8f58e42529f6e29ba0 "$replacement" |
    sha256sum --check --status || return 2
}

historical_validation_overlay_json() {
  printf '%s\n' '{"Replace":{"/workspace/thruput-go/chuckd/sub/baton/baton/vproposal.go":"/workspace/scripts/ci/historical-validation/vproposal.go"}}'
}

historical_validation_prepare() {
  [[ ${GIT_BRANCH:-} == codex/combined-native-multi-validation-phases ]] || return 0
  [[ ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
     ${HOG_HOST:-} =~ ^hog([4-9]|10|11)$ &&
     ${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
     ${GIT_COMMIT:-} =~ ^[a-f0-9]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" &&
     $(pwd -P) == /workspace ]] || return 2

  case $HOG_HOST in
  hog4|hog8)
    local overlay=/workspace/.ci/historical-validation-overlay
    local pinned=714ff40dcbbcbcb2cb851ce0de1440c37204d595
    local module=/workspace/thruput-go/chuckd
    [[ $(git -C "$module" rev-parse HEAD) == "$pinned" &&
       $(realpath -e /workspace/.ci) == /workspace/.ci ]] || return 2
    historical_validation_verify_sources || return 2
    [[ $(type -P go) == /usr/bin/go ]] || return 2
    mkdir -m 700 -- "$overlay" || return 2
    historical_validation_overlay_json >"$overlay/overlay.json" || return 2
    install -m 500 /workspace/scripts/ci/historical-validation/go "$overlay/go" || return 2
    export PATH="$overlay:$PATH"
    ;;
  esac

  case $HOG_HOST in
  hog4|hog7|hog8|hog11) export HOGD_HISTORICAL_VALIDATION_PHASES=1 ;;
  esac
}
