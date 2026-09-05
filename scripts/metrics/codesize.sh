#!/bin/bash

# Prints the byte size of a built contract's on-chain RV32 artifact image.

if [ $# -ne 2 ]; then
  echo "Usage: $0 <build_dir> <contract_name>"
  exit 1
fi

BUILD_DIR=$1
CONTRACT_NAME=$2

FILE="$BUILD_DIR/built_contracts/$CONTRACT_NAME.rv32a"
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
