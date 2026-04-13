#!/bin/sh

set -ex

for host in aws-hog0 aws-hog1 aws-hog2; do
  ssh $host "killall hogx-agent"
  ssh $host "pkill -f '^/tmp/hogx/pkg'"
done
