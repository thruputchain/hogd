#!/usr/bin/env bash
# Source this file to install a verified binary and configure the calling job.

hogd_install_sccache() {
  local version=0.18.0 target checksum install_dir temporary archive
  if command -v sccache >/dev/null 2>&1 && [[ "$(sccache --version)" == "sccache $version" ]]; then
    return 0
  fi
  case "$(uname -s)/$(uname -m)" in
  Linux/x86_64)
    target=x86_64-unknown-linux-musl
    checksum=45f1447fbe231e3037bde351ef70677dd212216c8d62ae7ca409fecc4d6acc89
    ;;
  Linux/aarch64)
    target=aarch64-unknown-linux-musl
    checksum=2b3284d5da3b46a47dc4229e75bb7b88ac4aa99c8d754fb7d2f84997e5a4354a
    ;;
  Darwin/arm64)
    target=aarch64-apple-darwin
    checksum=308184519b646f5125289e8515b36f6ca65a13a041923994aebe702348674e8e
    ;;
  Darwin/x86_64)
    target=x86_64-apple-darwin
    checksum=1dade83cc49eeb42337565eccd534b05982820a8e44b851bd7937467a18c7aef
    ;;
  *)
    echo 'Install sccache 0.18.0 manually on this CI architecture.' >&2
    return 1
    ;;
  esac
  install_dir="${XDG_CACHE_HOME:-${USER_HOME:-$HOME}/.cache}/hogd-tools/sccache/$version/$target"
  if [[ ! -x "$install_dir/sccache" ]]; then
    mkdir -p "$install_dir" || return
    temporary="$(mktemp -d "$install_dir/.install.XXXXXX")" || return
    archive="sccache-v$version-$target"
    if ! curl --fail --location --retry 3 --silent --show-error \
      "https://github.com/mozilla/sccache/releases/download/v$version/$archive.tar.gz" \
      --output "$temporary/archive.tar.gz" ||
      ! python3 - "$temporary/archive.tar.gz" "$checksum" <<'PY'
import hashlib
import pathlib
import sys

actual = hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).hexdigest()
if actual != sys.argv[2]:
    sys.exit("sccache release checksum mismatch")
PY
    then
      rm -rf -- "$temporary"
      return 1
    fi
    if ! tar -xzf "$temporary/archive.tar.gz" -C "$temporary" ||
      ! chmod 755 "$temporary/$archive/sccache" ||
      ! mv -f "$temporary/$archive/sccache" "$install_dir/sccache"; then
      rm -rf -- "$temporary"
      return 1
    fi
    rm -rf -- "$temporary"
  fi
  export PATH="$install_dir:$PATH"
}

hogd_install_sccache || return $?
unset -f hogd_install_sccache
source "$(dirname "${BASH_SOURCE[0]}")/../sccache-env.sh"
sccache --version
