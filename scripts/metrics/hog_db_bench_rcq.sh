if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

#Within the CI runners: the CI working directory is on the local SSD
CWD=$(pwd)

rm -rf $CWD/tmp
mkdir -p $CWD/tmp

$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp rcq -n 1000000 -w 24 | sed 's/^/Sequential: /'

rm -rf $CWD/tmp
mkdir -p $CWD/tmp

$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp rcq_concurrent -n 1000000 -w 24 | sed 's/^/Concurrent: /'

rm -rf $CWD/tmp
