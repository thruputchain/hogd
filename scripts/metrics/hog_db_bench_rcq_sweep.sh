if [ $# -ne 1 ]; then
  echo "Usage: $0 <build_dir>"
  exit 1
fi

BUILD_DIR=$1

#Within the CI runners: the CI working directory is on the local SSD
CWD=$(pwd)

# Entries per RCQ internal block (must match kRetrocausalQueueBlockEntriesMaxLen).
ENTRIES_PER_BLOCK=1000
# Number of blockchain blocks per run.  Keeping this constant across queue
# counts means throughput reflects queue parallelism rather than commit overhead.
BLOCKS=10
WORKERS=24
QUEUE_COUNTS="4 16 64 256 1024 4096"

for q in $QUEUE_COUNTS; do
  n=$((BLOCKS * q * ENTRIES_PER_BLOCK))

  rm -rf $CWD/tmp
  mkdir -p $CWD/tmp
  $BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp rcq -n $n -w $WORKERS -q $q | sed "s/^/q=${q} seq: /"

  rm -rf $CWD/tmp
  mkdir -p $CWD/tmp
  $BUILD_DIR/hogd/hog_db/hog_db_bench $CWD/tmp rcq_concurrent -n $n -w $WORKERS -q $q | sed "s/^/q=${q} conc: /"
done

rm -rf $CWD/tmp
