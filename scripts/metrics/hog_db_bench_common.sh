# Sourced by the Hog DB metric scripts. Each invocation starts with a fresh DB.
run_hog_db_bench() (
  set -e -o pipefail
  local label=$1
  shift
  local db_dir

  if [[ -n ${HOGD_CI_DB_BENCH_ROOT:-} ]]; then
    db_dir=$(mktemp -d -- "$HOGD_CI_DB_BENCH_ROOT/hog-db-bench.XXXXXXXX")
  else
    db_dir=$PWD/tmp
    rm -rf -- "$db_dir"
    mkdir -- "$db_dir"
  fi
  trap 'rm -rf -- "$db_dir"' EXIT

  "$BUILD_DIR/hogd/hog_db/hog_db_bench" "$db_dir" "$@" | sed "s/^/$label: /"
)
