#!/bin/bash

set -x

echo 'ensure git submodule cache is populated'
GIT_CACHE_PATH=/tmp/hogcache/

mkdir -p $GIT_CACHE_PATH

git config --global protocol.file.allow always

# cache_repo <repo_path>
# Example: cache_repo facebook/rocksdb
cache_repo() {
  local repo_path="$1"

  local cache_dir="$GIT_CACHE_PATH/$repo_path"

  if [[ ! -d "$cache_dir" ]]; then
    mkdir -p "$(dirname "$cache_dir")"
    git clone --mirror --quiet https://github.com/$repo_path.git "$cache_dir"
  fi

  git config --global \
    url."file://$cache_dir".insteadOf \
    https://github.com/$repo_path.git
}

# Cache the repos we need
cache_repo facebook/rocksdb
cache_repo gramseyer/model-checker
cache_repo google/googletest
cache_repo google/benchmark
cache_repo google/crc32c
cache_repo google/glog
cache_repo BLAKE3-team/BLAKE3
cache_repo corrosion-rs/corrosion
cache_repo pq-code-package/mldsa-native
cache_repo C2SP/wycheproof
