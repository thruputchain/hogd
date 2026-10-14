#!/bin/sh

set -ex

for host in hog1 hog2 hog3; do
  ssh $host "killall hogx-agent"
  ssh $host "pkill -f '^/tmp/hogx/pkg'"
done
