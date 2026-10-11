#define _GNU_SOURCE
#include <errno.h>
#include <linux/prctl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

// Diagnostic only: resize the kernel table after slow trials, then restore it.
// No functions in the benchmark or its libraries are interposed.
static pthread_t monitor;
static atomic_int stopping;
static int started;

static unsigned long long
now_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void
set_slots(unsigned long slots, unsigned rep)
{
  unsigned long long start = now_ns();
  errno = 0;
  int rc = prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_SET_SLOTS, slots, 0UL, 0UL);
  int err = errno;
  int actual = prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_GET_SLOTS, 0UL, 0UL, 0UL);
  fprintf(stderr,
          "FUTEXPROBE after_rep=%u requested=%lu actual=%d rc=%d errno=%d "
          "start_ns=%llu end_ns=%llu\n",
          rep, slots, actual, rc, err, start, now_ns());
}

static void *
watch_trials(void *unused)
{
  (void)unused;
  const char *path = getenv("HOG_FUTEX_TRIALS");
  FILE *in = NULL;
  while (!atomic_load(&stopping) && !(in = fopen(path, "r")))
    usleep(2000);
  if (!in)
    return NULL;
  char *line = NULL;
  size_t cap = 0;
  int first_slow = -1;
  int original = -1;
  int changed = 0;
  while (!atomic_load(&stopping)) {
    if (getline(&line, &cap, in) < 0) {
      clearerr(in);
      usleep(2000);
      continue;
    }
    unsigned rep, txs, threads;
    unsigned long long start, end, us;
    if (sscanf(line,
               "LOCKTRIAL rep=%u txs=%u threads=%u start_ns=%llu end_ns=%llu "
               "time_us=%llu",
               &rep, &txs, &threads, &start, &end, &us) != 6 ||
        txs != 100000 || threads != 2048)
      continue;
    if (first_slow < 0 && rep <= 2 && us > 400000) {
      first_slow = (int)rep;
      original = prctl(PR_FUTEX_HASH, PR_FUTEX_HASH_GET_SLOTS, 0UL, 0UL, 0UL);
      fprintf(stderr,
              "FUTEXPROBE detected_rep=%u original_slots=%d time_us=%llu\n",
              rep, original, us);
    }
    if (original >= 0 && !changed && rep == (unsigned)(first_slow + 2)) {
      set_slots(1048576UL, rep);
      changed = 1;
    }
    if (changed && rep == 6) {
      set_slots((unsigned long)original, rep);
      changed = 0;
      break;
    }
  }
  free(line);
  fclose(in);
  return NULL;
}

__attribute__((constructor)) static void
start_probe(void)
{
  if (getenv("HOG_FUTEX_TRIALS"))
    started = pthread_create(&monitor, NULL, watch_trials, NULL) == 0;
}

__attribute__((destructor)) static void
stop_probe(void)
{
  if (started) {
    atomic_store(&stopping, 1);
    pthread_join(monitor, NULL);
  }
}
