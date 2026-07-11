#!/bin/bash

# Prints the byte size of a built contract artifact.
# The extension selects the runtime target: "wasm" (default) measures the
# wasm module; "rv32a" measures the on-chain RV32 artifact image.

if [ $# -lt 2 ] || [ $# -gt 3 ]; then
  echo "Usage: $0 <build_dir> <contract_name> [extension]"
  exit 1
fi

BUILD_DIR=$1
CONTRACT_NAME=$2
EXT=${3:-wasm}

FILE="$BUILD_DIR/built_contracts/$CONTRACT_NAME.$EXT"
if [ ! -f "$FILE" ]; then
  echo "File not found: $FILE" >&2
  exit 2
fi

if [[ "$(uname)" == "Darwin" ]]; then
  SIZE=$(stat -f%z "$FILE")
elif [[ "$(uname)" == "Linux" ]]; then
  SIZE=$(stat -c%s "$FILE")
else
  echo "Unsupported OS: $(uname)" >&2
  exit 3
fi
echo $SIZE
