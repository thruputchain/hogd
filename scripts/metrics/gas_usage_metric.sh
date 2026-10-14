#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>" 1>&2
  exit 1
fi

BUILD_DIR=$1

$BUILD_DIR/hogd/loadgen/gas_usage_metric
