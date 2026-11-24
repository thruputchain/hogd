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

rcq_compact_mask() {
  [ "${HOGD_CI_CONTAINER:-}" = 1 ] &&
    [ "${HOGD_CI_OFFLINE:-}" = 1 ] &&
    [ "${HOG_HOST:-}" = hog10 ] &&
    [ "${GIT_BRANCH:-}" = main ] &&
    [ -z "${DEFAULT_THREADPOOL_SIZE+x}${DEFAULT_THREADPOOL_SIZE_hog_db_bench+x}" ] ||
    return 1
  printf '%s\n' "${HOG_XID:-}" |
    grep -Eq '^hogd-(combined-metrics|metrics)-[1-9][0-9]*-[1-9][0-9]*$' || return 1
  python3 "$(dirname "$0")/hog_db_rcq_cpu_mask.py"
}

if compact_mask=$(rcq_compact_mask) && taskset -c "$compact_mask" true; then
  status=0
  bash -o pipefail -c '
    taskset -c "$1" "$2" "$3" rcq_concurrent -n 1000000 -w 24 | sed "s/^/Concurrent: /"
  ' bash "$compact_mask" "$BUILD_DIR/hogd/hog_db/hog_db_bench" "$CWD/tmp" || status=$?
  rm -rf "$CWD/tmp"
  exit "$status"
fi

$BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp rcq_concurrent -n 1000000 -w 24 | sed 's/^/Concurrent: /'

rm -rf $CWD/tmp
