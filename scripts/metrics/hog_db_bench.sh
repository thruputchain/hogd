if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

#Within the CI runners: the CI working directory is on the local SSD
CWD=$(pwd)

rm -rf $CWD/tmp
mkdir -p $CWD/tmp

$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp roundrobin_payments -n 1000000 -w 24 | sed 's/^/Default: /'

rm -rf $CWD/tmp
mkdir -p $CWD/tmp
$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp roundrobin_payments -n 1000000 -w 24 -N | sed 's/^/ReplayCache: /'

rm -rf $CWD/tmp
mkdir -p $CWD/tmp
$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp roundrobin_payments -n 1000000 -w 24 -d | sed 's/^/DBHash: /'

rm -rf $CWD/tmp
mkdir -p $CWD/tmp
$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp roundrobin_payments -n 1000000 -w 24 -N -d | sed 's/^/ReplayCache+DBHash: /'

rm -rf $CWD/tmp
