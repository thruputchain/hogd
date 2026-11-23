#!/usr/bin/env bash
# Install a post-proposal timing Chuckd only inside the scoped hog4 workload.
set -Eeuo pipefail
fail() {
  printf 'Chuckd proposal timing build: %s\n' "$1" >&2
  exit 1
}
trap 'fail "failed at line $LINENO"' ERR
[[ $# == 0 && $(pwd -P) == /workspace &&
${GIT_BRANCH:-} == codex/combined-native-single-post-proposal &&
${HOG_HOST:-} == hog4 && ${HOGD_CI_CONTAINER:-} == 1 && ${HOGD_CI_OFFLINE:-} == 1 &&
${HOG_XID:-} =~ ^hogd-combined-metrics-[1-9][0-9]*-[1-9][0-9]*$ &&
${GIT_COMMIT:-} =~ ^[0-9a-f]{40}$ && $(git rev-parse HEAD) == "$GIT_COMMIT" ]] ||
  fail "requires exact offline historical Combined source on hog4"

# Match the qualified image's offline toolchain and the exact trusted script.
[[ ${GOPROXY:-} == off && ${GOSUMDB:-} == off && ${GOTOOLCHAIN:-} == local ]] || fail "offline Go environment missing"
chuckd=/workspace/thruput-go/chuckd
builder="$chuckd/scripts/build_chuckd.sh"
proposal="$chuckd/sub/baton/baton/propose.go"
commit="$chuckd/sub/baton/baton/commit.go"
patch=/workspace/scripts/ci/historical_chuckd_post_proposal.patch
[[ $(git -C "$chuckd" rev-parse HEAD) == 714ff40dcbbcbcb2cb851ce0de1440c37204d595 ]]
printf '%s  %s\n' \
  dd1bee01e84c85b929adfaea0d5b244023af9b7c0a6a8dbcc22f1f7ec26b2555 "$builder" \
  4c6b95b5ede3121fedbf4430c9397d281b7ec41335acc588472618b3e726426e "$commit" \
  6a40ac6f99369f6411023594794d4ce54797bcc360d53b353df68ff6e3da867d "$proposal" \
  33daf450f7b9bb473b53b868eb3162ad5dfc635370b6039eb609515480e32b03 "$patch" | sha256sum -c >&2
[[ ${GOFLAGS:-} != *-overlay* && ! -e $chuckd/chuckd/chuckd && ! -L $chuckd/chuckd/chuckd ]]
umask 077
scratch=$(mktemp -d /workspace/.ci/tmp/chuckd-proposal.XXXXXXXX)
trap 'rm -rf -- "$scratch"' EXIT
[[ -f release/chuckd.dev && -x release/chuckd.dev && ! -L release/chuckd.dev && -O ./release/chuckd.dev && $(stat -c %d "$scratch") == "$(stat -c %d release)" ]] || fail "unsafe binary or scratch ownership"
before=$(sha256sum release/chuckd.dev)
go version -m release/chuckd.dev | sed '1s/^[^[:blank:]]*[[:blank:]]*//' >"$scratch/before.meta"
mkdir -p "$scratch/sub/baton/baton"
cp -- "$proposal" "$commit" "$scratch/sub/baton/baton/"
git -C "$scratch" init -q
git -C "$scratch" apply --unidiff-zero --check "$patch"
git -C "$scratch" apply --unidiff-zero "$patch"
printf '%s  %s\n' \
  5c135512de057542932b796ed9996c6cdbdc7baf9560e8bfd2449e507b991cd7 "$scratch/sub/baton/baton/propose.go" \
  afd91c73718cebbd5ced628960005a5c2c197534cce7072716b2c43ecf2bc66f "$scratch/sub/baton/baton/commit.go" | sha256sum -c >&2
python3 - "$scratch" "$chuckd" <<'PYTHON'
import json
from pathlib import Path
import sys

scratch, source = map(Path, sys.argv[1:])
files = ("sub/baton/baton/propose.go", "sub/baton/baton/commit.go")
(scratch / "overlay.json").write_text(
    json.dumps({"Replace": {str(source / path): str(scratch / path) for path in files}})
)
PYTHON
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
