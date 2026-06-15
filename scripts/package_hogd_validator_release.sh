#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <build-dir> <output-dir>" >&2
  exit 1
fi

build_dir=$1
output_dir=$2

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

validator_dir="$staging_dir/hogd-validator-linux-x86_64"
hog_devnode_dir="$staging_dir/hog-devnode-linux-x86_64"
mkdir -p "$validator_dir/contracts" "$hog_devnode_dir/contracts"

copy_exec "$build_dir/hogd/hogdb/hogdb" "$validator_dir/hog-db"
copy_exec "$build_dir/hogd/hogexec/hogexec" "$validator_dir/hog-exec"
copy_exec "$build_dir/hogd/mempool/mempool_exec" "$validator_dir/hog-mempool"
copy_exec "$build_dir/hogd/auth/auth_exec" "$validator_dir/hog-auth"
copy_exec "$build_dir/hogd/event/event_exec" "$validator_dir/hog-event"
copy_exec "$build_dir/hogd/indexer/indexer_exec" "$validator_dir/hog-indexer"
copy_exec "$build_dir/hogd/devnode/hog-devnode" "$validator_dir/hog-devnode"
copy_exec "$build_dir/hogd/devnode/hog-devnode" "$hog_devnode_dir/hog-devnode"
copy_file "$build_dir/built_contracts/dev_wallet.rv32a" \
  "$validator_dir/contracts/dev_wallet.rv32a"
copy_file "$build_dir/built_contracts/dev_wallet.rv32a" \
  "$hog_devnode_dir/contracts/dev_wallet.rv32a"

tar -C "$validator_dir" -czf "$output_dir/hogd-validator-linux-x86_64.tar.gz" \
  contracts/dev_wallet.rv32a \
  hog-auth \
  hog-db \
  hog-devnode \
  hog-event \
  hog-exec \
  hog-indexer \
  hog-mempool

tar -C "$hog_devnode_dir" -czf "$output_dir/hog-devnode-linux-x86_64.tar.gz" \
  contracts/dev_wallet.rv32a \
  hog-devnode
