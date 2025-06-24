if [ $# -ne 1 ]; then
    echo "Usage: $0 <build_dir>"
    exit 1
fi

BUILD_DIR=$1

FILE="$BUILD_DIR/test_contracts/integration_test_contracts/fibonacci.wasm"
if [ ! -f "$FILE" ]; then
    echo "File not found: $FILE"
    exit 2
fi

SIZE=$(stat -f%z "$FILE")
echo $SIZE

