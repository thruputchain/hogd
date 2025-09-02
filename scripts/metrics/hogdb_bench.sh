if [ $# -ne 1 ]; then
    echo "Usage: $0 <build_dir>"
    exit 1
fi

BUILD_DIR=$1


#Within the CI runners: the CI working directory is on the local SSD
CWD=$(pwd)

rm -rf $CWD/tmp
mkdir -p $CWD/tmp

$BUILD_DIR/hogd/hogdb/hogdb_bench $CWD/tmp roundrobin_payments 1000000 24

rm -rf $CWD/tmp