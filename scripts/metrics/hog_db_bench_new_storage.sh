#!/usr/bin/env bash
set -e

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1
source "$(dirname "${BASH_SOURCE[0]}")/hog_db_bench_common.sh"

# The published Default series used the binary's default of 128 workers.
run_hog_db_bench Default new_storage -n 1000000 -w 128
run_hog_db_bench ReplayCache new_storage -n 1000000 -w 24 -N
run_hog_db_bench DBHash new_storage -n 1000000 -w 24 -d
run_hog_db_bench ReplayCache+DBHash new_storage -n 1000000 -w 24 -N -d
