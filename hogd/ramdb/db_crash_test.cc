// db_crash_test.cc - crash a ramdb and check recovery

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace hogd {

namespace {

constexpr size_t kKeys = 1 * 1024 * 1024;
constexpr size_t kPuts = 16 * kKeys;
constexpr size_t kValueSize = 128;
constexpr size_t kBatchSize = 16384;
constexpr size_t kIters = 128;
constexpr size_t kSyncsPerSnapshot = 16;
constexpr auto kDurationBetweenCrashes = std::chrono::duration<double>{5};

constexpr bool kRemoveAllFilesAtStart = false;
constexpr bool kNoFork = false;

void
fail(DBError err)
{
  panic("Unexpected error: {}", err);
}

std::unique_ptr<Database>
make_db(std::string_view path, size_t n, size_t value_size)
{
  // calculate a large enough heap size (goal: target ~half full).
  size_t heap_size =
      Database::estimate_heap_size(n * 2, sizeof(uint64_t), value_size);

  // calculate the bucket order (goal: load factor of 1-2).
  int bucket_order = std::countr_zero(std::bit_floor(n));

  // create the database.
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(path, heap_size, bucket_order);
  if (!db)
    fail(db.error());

  // create the benchmark class.
  return std::move(*db);
}

// delete the old database (if present).
void
remove_all_files(std::string_view path)
{
  if (kRemoveAllFilesAtStart) {
    Directory dir(path);
    ErrnoStatus<std::vector<std::string>> files = dir.list();
    if (files) {
      for (std::string_view file : *files) {
        if (!has_extension(file, "log") && !has_extension(file, "snap"))
          continue;
        ErrnoStatus<void> ret = dir.unlink(file);
        if (!ret)
          fail(to_db_error(ret.error()));
      }
    }
  }
}

struct Shared {
  std::atomic<size_t> put;
  uint64_t synced_lsn;
  size_t recovery_count;
  std::atomic<size_t> watchdog;
  std::atomic<bool> starting_up;
};

uint64_t extract_put_seqnum(std::span<const std::byte> value) {
  return *reinterpret_cast<const uint64_t*>(value.subspan(0, sizeof(uint64_t)).data());
}

void
ensure_recovered_keys_uptodate(hogd::Database &db, Shared &shared)
{
  size_t end = shared.put.load();
  size_t start = 0;
  if (end > kKeys)
    start = end - kKeys;

  info("checking that the results of puts [{}, {}) have expected values", start, end);

  std::vector<uint64_t> missing_keys{};
  bool failure = false;
  for (size_t i = start; i < end; i++) {
    shared.watchdog++;
    uint64_t key = i % kKeys;
    DBStatus<DBItem> ret = db.get(byte_view(key));
    if (!ret) [[unlikely]] {
      missing_keys.push_back(key);
      failure = true;
      continue;
    }

    // prefix of value should contain the put sequence number.
    // it's possible that a sync() could have completed but the process failed
    // to record that fact in shared memory; hence, it's possible that some
    // keys reflect the state of a later sync() than shared.put indicates.
    uint64_t found = extract_put_seqnum(ret->value);
    // either equal to the put number or the put number that key would have received in the next sync.
    if (found != i && found != i + kKeys) {
      error("checking put {} for key {} expected {} found {}", i, key, i, found);
      failure = true;
    }
  }

  std::ranges::sort(missing_keys);
  std::print("Missing keys: ");
  uint64_t prev = kKeys;
  bool last_skipped = false;
  for (uint64_t key : missing_keys) {
    if (prev + 1 == key) {
      last_skipped = true;
    } else {
      if (last_skipped)
        std::print(" ... ");
      std::print("{}", key);
      last_skipped = false;
    }
    prev = key;
  }
  if (last_skipped)
    std::print(" ... {}", prev);
  std::println();

  if (failure)
    panic("unexpected state after recovery");
}

size_t
db_size(hogd::Database &db)
{
  size_t size = 0;
  db.get_each([&size](std::span<const std::byte>, uint8_t, std::span<const std::byte>) { size++; });
  return size;
}

void
do_child(std::string_view path, Shared *shared)
{
  shared->starting_up = true;

  auto db = make_db(path, kKeys, kValueSize);
  warn("DB RECOVERED SYNCED_LSN IS {}", db->get_synced_lsn());

  ensure_recovered_keys_uptodate(*db, *shared);

  std::vector<std::byte> value{kValueSize};
  size_t left_off_put = shared->put.load();
  info("resuming put()s at {}; {} SUCCESSFUL RECOVERIES", left_off_put,
       shared->recovery_count);
  shared->recovery_count++;

  shared->starting_up = false;

  for (size_t put = left_off_put; put < kPuts; put++) {
    shared->watchdog++;
    uint64_t key = put % kKeys;
    std::ranges::copy(byte_view(put), value.begin());
    DBStatus<void> ret = db->put(byte_view(key), 0, std::span(value));
    if (!ret) [[unlikely]]
      fail(ret.error());

    if (put && (put % kBatchSize == 0)) {
      uint64_t lsn;
      if (shared->synced_lsn && (shared->synced_lsn % kSyncsPerSnapshot == 0)) {
        info("DATABASE SIZE JUST BEFORE SNAPSHOT: {}", db_size(*db));
        auto ret = db->sync_and_snapshot(0);
        if (!ret) [[unlikely]]
          fail(ret.error());
        lsn = ret->first;
        auto sync_ret = ret->second.get();
        if (!sync_ret)
          fail(sync_ret.error());
      }
      else {
        DBStatus<uint64_t> ret = db->sync();
        if (!ret) [[unlikely]]
          fail(ret.error());
        lsn = *ret;
      }

      info("child synced through lsn {} put {}", lsn, put);

      if (shared->synced_lsn + 1 < lsn) {
        warn("LSN IN SHM OUT OF DATE AFTER RECOVERY - UPDATING");
      }
      else if (shared->synced_lsn + 1 > lsn) {
        panic("FAILURE - LSN {} WAS LAST SYNCED ACCORDING TO SHM; NEW CALL TO "
              "sync() RETURNED LSN {}",
              shared->synced_lsn, lsn);
      }

      shared->synced_lsn = lsn;
      shared->put.store(put + 1);
      info("Recorded {} as the place to resume operation", shared->put.load());
    }
  }

  std::exit(0);
}

void
do_parent(pid_t child_pid, Shared *shared)
{
  using namespace std::chrono_literals;

  size_t start_watchdog = shared->watchdog.load();
  std::this_thread::sleep_for(kDurationBetweenCrashes);
  size_t end_watchdog = shared->watchdog.load();

  if (start_watchdog == end_watchdog) {
    error("!!! CHILD SEEMS STUCK !!! CHILD SEEMS STUCK!!! CHILD SEEMS STUCK!!! "
          "CHILD SEEMS STUCK !!!");
  }

  // before trying to kill the child, first see if it ran into some kind of
  // failure.
  int wstatus = 0;
  pid_t r = waitpid(child_pid, &wstatus, WNOHANG);
  if (r != 0 && r == child_pid && WIFEXITED(wstatus))
    goto dead;

  // child may already be dead, so ignore return.
  kill(child_pid, SIGKILL);

  r = wait(&wstatus);

dead:
  if (r == child_pid && WIFEXITED(wstatus)) {
    int exit_status = WEXITSTATUS(wstatus);
    error("child exited with status {}; exiting", exit_status);
    std::exit(exit_status);
  }

  if (shared->starting_up)
    panic("child didn't make it through startup before injected crash; exiting testing");

  info("restarting child");
}

} // namespace

} // namespace hogd

namespace {

hogd::Shared *
map_shared()
{
  void *p = mmap(nullptr, sizeof(hogd::Shared), PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  if (p == reinterpret_cast<void *>(-1))
    hogd::panic("mmap failed");

  return new (p) hogd::Shared{};
}

} // namespace

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  std::set_terminate([] { std::exit(EXIT_FAILURE); });

  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting db_crash_test!");

  auto print_usage = [] { std::println("usage: [db_path]"); };

  if (argc < 2) {
    print_usage();
    return 1;
  }

  std::string path{argv[1]};

  hogd::remove_all_files(path);
  hogd::Shared *shared = map_shared();

  if (hogd::kNoFork) {
    do_child(path, shared);
    std::exit(0);
  }

  for (size_t iter = 0; iter < hogd::kIters; iter++) {
    pid_t pid = fork();
    if (pid == -1)
      hogd::panic("failed to create child process");

    if (pid == 0)
      hogd::do_child(path, shared);
    else
      hogd::do_parent(pid, shared);
  }

  hogd::info("ALL DONE - EXITING");

  return 0;
}
