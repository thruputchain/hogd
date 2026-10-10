#!/usr/bin/env bash
set -Eeuo pipefail
cd /tmp/hogjob-hogci
read -r variant label recycle <ab-active.txt
mkdir -p ab_profiles
echo "$$" >ab-server.pid
if [[ $recycle == off ]]; then
  export HOG_LOCK_NO_RECYCLE=1
else
  unset HOG_LOCK_NO_RECYCLE
fi
exec "./release/hogd/hog_exec/bench/bench_server.$variant" "$@" 2>"ab_profiles/$label-db.log"
