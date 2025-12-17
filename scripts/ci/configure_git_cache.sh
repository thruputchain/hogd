#!/bin/bash

set -x

echo 'ensure git submodule cache is populated'
GIT_CACHE_PATH=/tmp/hogcache/

mkdir -p $GIT_CACHE_PATH

if [[ ! -d $GIT_CACHE_PATH/facebook/rocksdb ]]; then
    git clone --mirror https://github.com/facebook/rocksdb.git $GIT_CACHE_PATH/facebook/rocksdb.git
fi

git config --global \
  url."file://$GIT_CACHE_PATH/facebook/rocksdb.git".insteadOf \
  https://github.com/facebook/rocksdb.git