#!/bin/bash

if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

# Full rv32_vm_bench output (all benchmarks): canonical/threaded interpreters,
# fibonacci + compute loops, wasmi comparison, and artifact benchmarks.
"$BUILD_DIR/hogd/hogexec/runtime/rv32/rv32_vm_bench"
