if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

#Within the CI runners: the CI working directory is on the local SSD
CWD=$(pwd)

rm -rf $CWD/tmp
mkdir -p $CWD/tmp

$BUILD_DIR/hogd/hogdb/hogdb_bench $CWD/tmp onetomany_payments 1000000 24 0 | sed 's/^/Default: /'

rm -rf $CWD/tmp
mkdir -p $CWD/tmp

$BUILD_DIR/hogd/hogdb/hogdb_bench $CWD/tmp onetomany_payments 1000000 24 1 | sed 's/^/ReplayCache: /'

rm -rf $CWD/tmp
