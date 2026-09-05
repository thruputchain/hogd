#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 4 ]]; then
  echo "usage: $0 <build-dir> <output-dir> [flavor] [architecture]" >&2
  exit 1
fi

build_dir=$1
output_dir=$2
flavor=${3:-release}
architecture=${4:-x86_64}

case "$architecture" in
x86_64 | arm64) ;;
*)
  echo "error: unsupported architecture: $architecture" >&2
  exit 1
  ;;
esac

case "$flavor" in
release)
  validator_package="hogd-validator-linux-$architecture"
  hog_devnode_package="hog-devnode-linux-$architecture"
  ;;
debug)
  validator_package="hogd-validator-debug-linux-$architecture"
  hog_devnode_package="hog-devnode-debug-linux-$architecture"
  ;;
*)
  echo "error: unsupported flavor: $flavor" >&2
  exit 1
  ;;
esac

copy_exec() {
  local src=$1
  local dest=$2

  if [[ ! -x "$src" ]]; then
    echo "error: missing executable: $src" >&2
    exit 1
  fi

  cp "$src" "$dest"
  chmod +x "$dest"
}

copy_file() {
  local src=$1
  local dest=$2

  if [[ ! -f "$src" ]]; then
    echo "error: missing file: $src" >&2
    exit 1
  fi

  cp "$src" "$dest"
}

mkdir -p "$output_dir"
staging_dir=$(mktemp -d)
trap 'rm -rf "$staging_dir"' EXIT

validator_dir="$staging_dir/$validator_package"
hog_devnode_dir="$staging_dir/$hog_devnode_package"
mkdir -p "$validator_dir/contracts" "$hog_devnode_dir/contracts"

copy_exec "$build_dir/hogd/hog_db/hog-db" "$validator_dir/hog-db"
copy_exec "$build_dir/hogd/hog_exec/hog-exec" "$validator_dir/hog-exec"
copy_exec "$build_dir/hogd/hog_mempool/hog-mempool" "$validator_dir/hog-mempool"
copy_exec "$build_dir/hogd/hog_auth/hog-auth" "$validator_dir/hog-auth"
copy_exec "$build_dir/hogd/hog_event/hog-event" "$validator_dir/hog-event"
copy_exec "$build_dir/hogd/hog_indexer/hog-indexer" "$validator_dir/hog-indexer"
copy_exec "$build_dir/hogd/devnode/hog-devnode" "$validator_dir/hog-devnode"
copy_exec "$build_dir/hogd/devnode/hog-hosted-bootstrap" \
  "$validator_dir/hog-hosted-bootstrap"
copy_exec "$build_dir/hogd/devnode/hog-devnode" "$hog_devnode_dir/hog-devnode"
copy_file "$build_dir/built_contracts/dev_wallet.rv32a" \
  "$validator_dir/contracts/dev_wallet.rv32a"
copy_file "$build_dir/built_contracts/dev_wallet.rv32a" \
  "$hog_devnode_dir/contracts/dev_wallet.rv32a"
copy_file "$build_dir/built_contracts/genesis_root.rv32a" \
  "$validator_dir/contracts/genesis_root.rv32a"
copy_file "$build_dir/built_contracts/genesis_root.rv32a" \
  "$hog_devnode_dir/contracts/genesis_root.rv32a"

tar -C "$validator_dir" -czf "$output_dir/$validator_package.tar.gz" \
  contracts/dev_wallet.rv32a \
  contracts/genesis_root.rv32a \
  hog-auth \
  hog-db \
  hog-devnode \
  hog-event \
  hog-exec \
  hog-hosted-bootstrap \
  hog-indexer \
  hog-mempool

tar -C "$hog_devnode_dir" -czf "$output_dir/$hog_devnode_package.tar.gz" \
  contracts/dev_wallet.rv32a \
  contracts/genesis_root.rv32a \
  hog-devnode
