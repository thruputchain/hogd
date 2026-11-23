#!/usr/bin/env bash
# Install a proposal-timing Chuckd only inside the scoped hog4 workload.
set -Eeuo pipefail
fail() {
  printf 'Chuckd proposal timing build: %s\n' "$1" >&2
  exit 1
}
trap 'fail "failed at line $LINENO"' ERR
[[ $# == 0 && $(pwd -P) == /workspace &&
(${GIT_BRANCH:-} == codex/asan-chuckd-propose-timing ||
${GIT_BRANCH:-} == codex/asan-chuckd-rpc-breakdown ||
${GIT_BRANCH:-} == codex/asan-chuckd-proposal-receive ||
${GIT_BRANCH:-} == codex/asan-chuckd-tcp-info) &&
${HOG_HOST:-} == hog4 && ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
${HOG_XID:-} =~ ^hogd-mempool-bench-[1-9][0-9]*-[1-9][0-9]*$ &&
${GIT_COMMIT:-} =~ ^[0-9a-f]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" ]] ||
  fail "requires exact offline standalone ASAN source on hog4"

# Match the qualified image's offline toolchain and the exact trusted script.
[[ ${GOPROXY:-} == off && ${GOSUMDB:-} == off && ${GOTOOLCHAIN:-} == local ]] || fail "offline Go environment missing"
chuckd=/workspace/thruput-go/chuckd
builder="$chuckd/scripts/build_chuckd.sh"
proposal="$chuckd/sub/baton/baton/propose.go"
patch=/workspace/scripts/ci/asan_chuckd_propose.patch
[[ $(git -C "$chuckd" rev-parse HEAD) == 714ff40dcbbcbcb2cb851ce0de1440c37204d595 ]]
printf '%s  %s\n' \
  dd1bee01e84c85b929adfaea0d5b244023af9b7c0a6a8dbcc22f1f7ec26b2555 "$builder" \
  6a40ac6f99369f6411023594794d4ce54797bcc360d53b353df68ff6e3da867d "$proposal" \
  185b7656bd29b41548840b10f8b3ecdff7ecddfcf86696e02e0e6abd15e961b5 "$patch" | sha256sum -c >&2
[[ ${GOFLAGS:-} != *-overlay* && ! -e $chuckd/chuckd/chuckd && ! -L $chuckd/chuckd/chuckd ]]
umask 077
scratch=$(mktemp -d /workspace/.ci/tmp/chuckd-proposal.XXXXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
[[ -f release/chuckd.dev && -x release/chuckd.dev && ! -L release/chuckd.dev && -O ./release/chuckd.dev && $(stat -c %d "$scratch") == "$(stat -c %d release)" ]] || fail "unsafe binary or scratch ownership"
before=$(sha256sum release/chuckd.dev)
go version -m release/chuckd.dev | sed '1s/^[^[:blank:]]*[[:blank:]]*//' >"$scratch/before.meta"
mkdir -p "$scratch/sub/baton/baton"
replacement="$scratch/sub/baton/baton/propose.go"
cp -- "$proposal" "$replacement"
git -C "$scratch" init -q
git -C "$scratch" apply --unidiff-zero --check "$patch"
git -C "$scratch" apply --unidiff-zero "$patch"
printf '%s  %s\n' 0913591b5ff676b272d1e00f0d82627038591f6e2e0e2382e6e04c27b66792ab "$replacement" |
  sha256sum -c >&2
overlay_paths=("$proposal" "$replacement")
if [[ $GIT_BRANCH == codex/asan-chuckd-proposal-receive || $GIT_BRANCH == codex/asan-chuckd-tcp-info ]]; then
  scheduler="$chuckd/sub/sched/schedcoord.go"
  receive_patch=/workspace/scripts/ci/asan_chuckd_receive.patch
  printf '%s  %s\n' \
    9108fc60b2742d9f0b862d227a81cb896decdf2bbdc67788cab9b70b0ef9aec4 "$scheduler" \
    324081fccee52dd61e1d8e20175c3cd80c8b32daa5f03b6eab3166cf1f923fac "$receive_patch" | sha256sum -c >&2
  mkdir -p "$scratch/sub/sched"
  receive_replacement="$scratch/sub/sched/schedcoord.go"
  cp -- "$scheduler" "$receive_replacement"
  git -C "$scratch" apply --unidiff-zero --check "$receive_patch"
  git -C "$scratch" apply --unidiff-zero "$receive_patch"
  printf '%s  %s\n' 82091c2bb372a443f0f824097ca325036a5126ab46205d04310c6c3fd3b90fd9 "$receive_replacement" |
    sha256sum -c >&2
  overlay_paths+=("$scheduler" "$receive_replacement")
fi
python3 - "$scratch/overlay.json" "${overlay_paths[@]}" <<'PYOVERLAY'
import json
from pathlib import Path
import sys

manifest, *paths = sys.argv[1:]
Path(manifest).write_text(json.dumps({"Replace": dict(zip(paths[::2], paths[1::2], strict=True))}))
PYOVERLAY
# Run the original script as-is. Stock race/CGO/ldflags/module settings must
# exactly match the originally built executable before replacing it.
(
  cd "$scratch"
  export GOFLAGS="${GOFLAGS:+$GOFLAGS }-overlay=$scratch/overlay.json"
  timeout -k 5 180 "$builder"
) >"$scratch/build.log" 2>&1 || {
  fail "overlay compiler failed; the stock executable was preserved"
}
go version -m "$scratch/chuckd.dev" | sed '1s/^[^[:blank:]]*[[:blank:]]*//' >"$scratch/after.meta"
[[ -s $scratch/before.meta && -s $scratch/after.meta ]]
if ! cmp -s "$scratch/before.meta" "$scratch/after.meta"; then
  echo "Chuckd build metadata differs from the stock executable." >&2
  exit 1
fi
after=$(sha256sum "$scratch/chuckd.dev")
[[ ${before%% *} != "${after%% *}" ]] || fail "overlay did not change the binary"
mv -T -- "$scratch/chuckd.dev" release/chuckd.dev
installed=$(sha256sum release/chuckd.dev)
[[ ${installed%% *} == "${after%% *}" ]] || fail "installed binary does not match the built binary"
metadata=$(sha256sum "$scratch/after.meta")
printf 'host=hog4\nsource=%s\nxid=%s\nchuckd_revision=714ff40dcbbcbcb2cb851ce0de1440c37204d595\nstock_sha256=%s\neffective_sha256=%s\ngo_metadata_sha256=%s\n' "$GIT_COMMIT" "$HOG_XID" "${before%% *}" "${after%% *}" "${metadata%% *}"
