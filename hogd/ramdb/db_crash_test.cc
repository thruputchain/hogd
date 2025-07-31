// db_crash_test.cc - crash a ramdb and check recovery

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
// NOLINTNEXTLINE(modernize-deprecated-headers)
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/string.h"
#include "hogd/base/time.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/metrics/metrics.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

using namespace std::chrono_literals;

constexpr size_t kKeys = 16 * 1024 * 1024;
static_assert((kKeys & (kKeys - 1)) == 0, "kKeys must be a power of two");
constexpr size_t kPuts = 16 * kKeys;
static_assert((kPuts & (kPuts - 1)) == 0, "kPuts must be a power of two");
constexpr size_t kValueSize = 128;
constexpr size_t kBatchSize = 128 * 1024;
constexpr size_t kSyncsPerSnapshot = 8;
constexpr auto kDurationBetweenCrashes = std::chrono::duration<double>{30};

constexpr bool kOverlapPutsWithSnapshot = true;
constexpr bool kRemoveAllFilesAtStart = false;
constexpr bool kDumpMetrics = true;
constexpr bool kNoFork = false;

// number of incremental snapshots before next full snapshot
// max() means always incremental, 0 means always full
constexpr size_t kIncrementalSnapshots = 4;

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
  uint64_t most_recent_snapshot_end_lsn{};
  uint64_t next_snapshot_begin_lsn{};
  size_t last_dump_recovery_count;
  size_t snapshots_until_full{kIncrementalSnapshots};
};

void
ensure_recovered_keys_uptodate(hogd::Database &db, Shared &shared)
{
  size_t end = shared.put.load();
  size_t start = 0;
  if (end > kKeys)
    start = end - kKeys;

  info("checking that puts [{}, {}) have expected values", start, end);

  std::vector<uint64_t> missing_keys{};
  bool failure = false;
  std::string failure_cause{};
  for (size_t i = start; i < end; i++) {
    shared.watchdog++;
    uint64_t key = i & (kKeys - 1);
    DBStatus<DBItem> ret = db.get(byte_view(key));
    if (!ret) [[unlikely]] {
      missing_keys.push_back(key);
      failure = true;
      failure_cause = std::format("missing key {}", key);
      continue;
    }

    // prefix of value should contain the put sequence number.
    // it's possible that a sync() could have completed but the process failed
    // to record that fact in shared memory; hence, it's possible that some
    // keys reflect the state of a later sync() than shared.put indicates.
    uint64_t found;
    std::ranges::copy(ret->value, writable_byte_view(found).begin());
    // either equal to the put number or the put number that key would have
    // received in the next sync.
    if (found != i && found != i + kKeys) {
      error("-> put {} for key {} expected {} found {}", i, key, i, found);
      failure = true;
      failure_cause = std::format("unexpected value for {}", key);
    }
  }

  std::ranges::sort(missing_keys);
  if (!missing_keys.empty())
    std::print("Missing keys: ");
  uint64_t prev = kKeys;
  bool last_skipped = false;
  for (uint64_t key : missing_keys) {
    if (prev + 1 == key) {
      last_skipped = true;
    }
    else {
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
    panic("unexpected state after recovery: {}", failure_cause);
}

size_t
db_size(hogd::Database &db)
{
  size_t size = 0;
  db.get_each([&size](std::span<const std::byte>, uint8_t,
                      std::span<const std::byte>) { size++; });
  return size;
}

using time_point = std::chrono::high_resolution_clock::time_point;

std::vector<Sample> prev_samples;
time_point start;

void
dump_metrics(MetricRegistry &registry,
             std::unordered_set<std::string_view> const &cumulative_fields,
             Shared &shared)
{
  if (!kDumpMetrics)
    return;

  std::vector<Sample> samples = registry.read();

  time_point now = std::chrono::high_resolution_clock::now();
  size_t ns = (now - start).count();

  info("== begin metrics ==");
  for (Sample &sample : samples)
    info("> {} {} {}", ns, sample.key, sample.val);
  info("== end metrics ==");

  bool skip_diff = shared.last_dump_recovery_count != shared.recovery_count;
  shared.last_dump_recovery_count = shared.recovery_count;

  if (!skip_diff) {
    std::vector<Sample> diff = samples;

    if (diff.size() == prev_samples.size()) {
      for (size_t i = 0; i < diff.size(); ++i) {
        if (cumulative_fields.contains(diff[i].key)) {
          diff[i].val -= prev_samples[i].val;
        }
      }
    }

    info("== begin delta metrics ==");
    for (Sample &sample : diff)
      info("Δ {} {} {} {}/s", ns, sample.key, sample.val,
           print_iec_bytes(sample.val, true));
    info("== end delta metrics ==");
  }

  prev_samples = std::move(samples);
}

void
do_child(std::string_view path, Shared &shared)
{
  MetricRegistry registry{};

  auto db = make_db(path, kKeys, kValueSize);

  db->register_metrics(registry);
  std::jthread _([&registry, &shared](std::stop_token &&stoken) {
    // this is expensive to build. should only do it once, worst case.
    std::vector<Metric> metrics = registry.get_metrics();
    // note cumulative_fields is borrowing views of strings in metrics.
    std::unordered_set<std::string_view> cumulative_fields =
        metrics |
        std::views::filter([](const Metric &m) { return m.cumulative; }) |
        std::views::transform(
            [](const Metric &m) { return std::string_view{m.key}; }) |
        std::ranges::to<std::unordered_set<std::string_view>>();

    while (!stoken.stop_requested()) {
      std::this_thread::sleep_for(1s);
      dump_metrics(registry, cumulative_fields, shared);
    }
  });

  ensure_recovered_keys_uptodate(*db, shared);

  std::vector<std::byte> value{kValueSize};
  size_t left_off_put = shared.put.load();
  info("--> resuming put()s at {}; {} successful recoveries", left_off_put,
       shared.recovery_count);
  shared.recovery_count++;

  shared.starting_up = false;

  DBStatus<std::pair<uint64_t, std::future<DBStatus<void>>>> snap_start_ret;
  auto wait_for_snapshot = [&snap_start_ret, &shared](bool blocking) {
    auto &future = snap_start_ret->second;

    if (!future.valid())
      return;

    // NOLINTNEXTLINE(misc-include-cleaner)
    if (!blocking && future.wait_for(0s) != std::future_status::ready)
      return;

    auto snap_end_ret = future.get();
    if (!snap_end_ret)
      fail(snap_end_ret.error());

    shared.next_snapshot_begin_lsn = shared.most_recent_snapshot_end_lsn;

    info("sync_and_snapshot finished");
  };

  for (size_t put = left_off_put; put < kPuts; put++) {
    // bump this to reassure parent we aren't wedged.
    shared.watchdog++;

    // issue the next put accoring to its put sequence number.
    uint64_t key = put & (kKeys - 1);
    std::ranges::copy(byte_view(put), value.begin());
    DBStatus<void> ret = db->put(byte_view(key), 0, std::span(value));
    if (!ret) [[unlikely]]
      fail(ret.error());

    // if an async snapshot is ongoing, see if it's return is ready.
    wait_for_snapshot(false);

    // sync after each batch.
    if (put && ((put & (kBatchSize - 1)) == 0)) {
      uint64_t lsn;

      // after enough syncs issue a full snapshot.
      if (shared.synced_lsn &&
          ((shared.synced_lsn & (kSyncsPerSnapshot - 1)) == 0)) {
        debug("Database size just before snapshot: {}", db_size(*db));

        // ensure we don't start two concurrent snapshots.
        wait_for_snapshot(true);

        uint64_t begin_lsn = shared.next_snapshot_begin_lsn;
        if (shared.snapshots_until_full == 0) {
          info("taking a full snapshot");
          begin_lsn = 0;
        }
        else {
          info("taking an incremental snapshot");
        }

        ElapsedTimer timer{};
        snap_start_ret = db->sync_and_snapshot(begin_lsn);
        if (!snap_start_ret) [[unlikely]]
          fail(snap_start_ret.error());
        lsn = snap_start_ret->first;

        shared.most_recent_snapshot_end_lsn = lsn;
        if (shared.snapshots_until_full == 0)
          shared.snapshots_until_full = kIncrementalSnapshots;
        else
          shared.snapshots_until_full--;

        info("sync_and_snapshot start took {:.2f} ms", timer.elapsed() * 1000);

        wait_for_snapshot(!kOverlapPutsWithSnapshot);
      }
      else {
        ElapsedTimer timer{};

        DBStatus<uint64_t> ret = db->sync();
        if (!ret) [[unlikely]]
          fail(ret.error());
        lsn = *ret;

        debug("sync took {:.2f} ms", timer.elapsed() * 1000);
      }

      debug("child synced through lsn {} put {}", lsn, put);

      if (shared.synced_lsn + 1 > lsn) {
        panic("FAILURE - LSN {} WAS LAST SYNCED ACCORDING TO SHM; NEW CALL TO "
              "sync() RETURNED LSN {}",
              shared.synced_lsn, lsn);
      }

      info("Updated synced_lsn to {}", lsn);
      shared.synced_lsn = lsn;
      shared.put.store(put + 1);
    }
  }

  std::exit(0);
}

void
do_parent(pid_t child_pid, Shared &shared)
{
  // for large dbs, rechecking that all of the keys have the expected values
  // can take a long time. so don't start the countdown to fault injection
  // until the start up phase is done to ensure some progres per crash.
  while (shared.starting_up)
    // NOLINTNEXTLINE(misc-include-cleaner)
    std::this_thread::sleep_for(1s);

  info("child has started up");

  size_t watchdog = shared.watchdog.load();
  std::this_thread::sleep_for(kDurationBetweenCrashes);

  if (watchdog == shared.watchdog.load())
    error("!!! CHILD SEEMS STUCK !!!");

  // before trying to kill the child, first see if it ran into some kind of
  // failure.
  int wstatus = 0;
  pid_t r = waitpid(child_pid, &wstatus, WNOHANG);
  if (r != 0 && r == child_pid && WIFEXITED(wstatus))
    goto dead;

  info("killing child");
  // child may already be dead, so ignore return.
  kill(child_pid, SIGKILL);

  r = wait(&wstatus);

dead:
  if (r == child_pid && WIFEXITED(wstatus)) {
    int exit_status = WEXITSTATUS(wstatus);
    error("child exited with status {}; exiting", exit_status);
    std::exit(exit_status);
  }

  if (shared.starting_up)
    panic("child didn't make it through startup before injected crash; exiting "
          "testing");

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
  if (reinterpret_cast<intptr_t>(p) == -1)
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

  // map structure between parent/child. we just leak the mapping.
  hogd::Shared &shared = *map_shared();

  while (true) {
    // set this in the parent, so parent knows once false, child must have
    // entered/exited start up phase.
    shared.starting_up = true;

    if (hogd::kNoFork)
      hogd::do_child(path, shared); // never returns

    pid_t pid = fork();
    if (pid == -1)
      hogd::panic("failed to create child process");

    if (pid == 0)
      hogd::do_child(path, shared); // never returns
    else
      hogd::do_parent(pid, shared); // returns to recover or exits if done.
  }

  std::unreachable();
}
