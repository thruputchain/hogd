#!/usr/bin/env bash

set -ex

# kill everything on all hosts in parallel
for i in $(seq 0 15); do
  host="aws-hog$i"
  ssh "$host" '
    killall hogx-agent &&
    pkill -f "^/tmp/hogx/pkg" || true
  ' &
done
wait
