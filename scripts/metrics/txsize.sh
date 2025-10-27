#!/bin/bash

if [ $# -ne 2 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

$BUILD_DIR/hogd/loadgen/tx_size_stats
