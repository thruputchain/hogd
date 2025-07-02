#!/bin/bash

if [ $# -ne 1 ]; then
    echo "Usage: $0 <build_dir>"
    exit 1
fi

BUILD_DIR=$1

FILE="$BUILD_DIR/test_contracts/fibonacci.wasm"
if [ ! -f "$FILE" ]; then
    echo "File not found: $FILE"
    exit 2
fi

if [[ "$(uname)" == "Darwin" ]]; then
    SIZE=$(stat -f%z "$FILE")
elif [[ "$(uname)" == "Linux" ]]; then
    SIZE=$(stat -c%s "$FILE")
else
    echo "Unsupported OS: $(uname)"
    exit 3
fi
echo $SIZE

