#!/bin/sh

set -ex

for host in aws-hog{0..15}; do
  ssh $host "killall hogx-agent"
  ssh $host "pkill -f '^/tmp/hogx/pkg'"
done
