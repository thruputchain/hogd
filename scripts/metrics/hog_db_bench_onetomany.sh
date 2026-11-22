#!/usr/bin/env bash
set -e

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1
source "$(dirname "${BASH_SOURCE[0]}")/hog_db_bench_common.sh"

run_hog_db_bench Default onetomany_payments -n 1000000 -w 24
run_hog_db_bench ReplayCache onetomany_payments -n 1000000 -w 24 -N
run_hog_db_bench DBHash onetomany_payments -n 1000000 -w 24 -d
run_hog_db_bench ReplayCache+DBHash onetomany_payments -n 1000000 -w 24 -N -d
