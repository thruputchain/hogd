// db_bench.cc - benchmarks for the database subsystem

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <numeric>
#include <print>
#include <ratio>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/os.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/ramdb/constants.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"
#include "hogd/ramdb/disk_writer.h"

namespace hogd {

namespace {

std::string db_path;

void
fail(DBError err)
{
  panic("Unexpected error: {}", err);
}

// measure returns the execution time of fn in seconds (as a double)
template<typename Func>
double
measure(Func &&fn)
{
  using clock = std::chrono::high_resolution_clock;
  clock::time_point start = clock::now();
  std::forward<Func>(fn)();
  clock::time_point end = clock::now();
  return std::chrono::duration<double>(end - start).count();
}

constexpr uint64_t
make_key(int a, int b)
{
  return static_cast<uint64_t>(a) | (static_cast<uint64_t>(b) << 32);
}

class Benchmark {
public:
  Benchmark(int num, int threads, int value_size, std::unique_ptr<Database> db)
    : num_(num), threads_(threads), value_size_(value_size), db_(std::move(db))
  {}
  ~Benchmark() = default;

  // write_items writes all the kv-pairs in the benchmark.
  void write_items();
  // read_items reads all the kv-pairs in the benchmark.
  void read_items();
  // delete_items deletes all the kv-pairs in the benchmark.
  void delete_items();
  // sync flushes and synchronizes all changes to disk.
  uint64_t sync();
  // last_sync_stats returns the stats of the last sync().
  SyncStats last_sync_stats() { return db_->last_sync_stats().value(); }
  // initializes the database with items (initial state for benchmarks).
  void prefill();
  // closes the DB and reloads all of its contents from disk.
  void reload_db(size_t num);
  // triggers a snapshot.
  void snapshot();
  // syncs and starts a snapshot, returning a future for it.
  std::pair<uint64_t, std::future<DBStatus<void>>> sync_and_snapshot();

private:
  // the number of kv-pairs to access
  const int num_;
  // the number of threads to use concurrently
  const int threads_;
  // the size of each kv-pair's value
  const int value_size_;

  // database instance
  std::unique_ptr<Database> db_;

  template<typename Func> void invoke_on_threads(const Func &fn) const
  {
    std::vector<std::jthread> workers;
    workers.reserve(threads_);
    for (int i = 0; i < threads_; i++) {
      workers.emplace_back(fn, i, num_ / threads_);
    }
  }
};

void
Benchmark::write_items()
{
  invoke_on_threads([this](int thread_id, int samples) mutable {
    std::vector<std::byte> value(value_size_);
    for (int i = 0; i < samples; ++i) {
      DBStatus<void> ret =
          db_->put(byte_view(make_key(i, thread_id)), 0, std::span(value));
      if (unlikely(!ret))
        fail(ret.error());
    }
  });
}

void
Benchmark::read_items()
{
  invoke_on_threads([this](int thread_id, int samples) mutable {
    for (int i = 0; i < samples; ++i) {
      DBStatus<DBItem> ret = db_->get(byte_view(make_key(i, thread_id)));
      if (unlikely(!ret))
        fail(ret.error());
    }
  });
}

void
Benchmark::delete_items()
{
  invoke_on_threads([this](int thread_id, int samples) mutable {
    for (int i = 0; i < samples; ++i) {
      DBStatus<void> ret = db_->remove(byte_view(make_key(i, thread_id)));
      if (unlikely(!ret))
        fail(ret.error());
    }
  });
}

uint64_t
Benchmark::sync()
{
  DBStatus<uint64_t> ret = db_->sync();
  if (unlikely(!ret))
    fail(ret.error());
  return ret.value();
}

void
Benchmark::prefill()
{
  write_items();
  sync();
}

void
Benchmark::reload_db(size_t num)
{
  size_t heap_size =
      Database::estimate_heap_size(num * 2, sizeof(uint64_t), value_size_);
  int bucket_order = std::countr_zero(std::bit_floor(num));
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(db_path, heap_size, bucket_order);
  if (!db)
    fail(db.error());
  db_ = std::move(*db);
}

void
Benchmark::snapshot()
{
  auto ret = db_->sync_and_snapshot(0);
  if (unlikely(!ret))
    fail(ret.error());
  DBStatus<void> r = ret->second.get();
  if (unlikely(!r))
    fail(r.error());
}

std::pair<uint64_t, std::future<DBStatus<void>>>
Benchmark::sync_and_snapshot()
{
  auto ret = db_->sync_and_snapshot(0);
  if (unlikely(!ret))
    fail(ret.error());
  return std::move(*ret);
}

std::unique_ptr<Benchmark>
make_benchmark(size_t n, int threads, size_t value_size,
               SyncStatsMode sync_stats = SyncStatsMode::kDisabled)
{
  // delete the old database (if present).
  Directory dir(db_path);
  ErrnoStatus<std::vector<std::string>> files = dir.list();
  if (files) {
    for (std::string_view file : *files) {
      if (!has_extension(file, "log"))
        continue;
      ErrnoStatus<void> ret = dir.unlink(file);
      if (!ret)
        fail(to_db_error(ret.error()));
    }
  }

  // calculate a large enough heap size (goal: target ~half full).
  size_t heap_size =
      Database::estimate_heap_size(n * 2, sizeof(uint64_t), value_size);

  // calculate the bucket order (goal: load factor of 1-2).
  int bucket_order = std::countr_zero(std::bit_floor(n));

  // create the database.
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(db_path, heap_size, bucket_order, kDiskLogFileThreshold,
                     sync_stats);
  if (!db)
    fail(db.error());

  // create the benchmark class.
  return std::make_unique<Benchmark>(n, threads, value_size, std::move(*db));
}

double
do_fill(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  return measure([&benchmark] {
    benchmark->write_items();
    benchmark->sync();
  });
}

double
do_replacements(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill();
  return measure([&benchmark] {
    benchmark->write_items();
    benchmark->sync();
  });
}

double
do_reads(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill();
  return measure([&benchmark] { benchmark->read_items(); });
}

double
do_deletions(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill();
  return measure([&benchmark] {
    benchmark->delete_items();
    benchmark->sync();
  });
}

double
do_reload(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill();
  return measure([&benchmark, num] { benchmark->reload_db(num); });
}

double
do_snapshot(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill();
  DBStatus<uint64_t> ret = benchmark->sync();
  if (unlikely(!ret))
    fail(ret.error());

  return measure([&benchmark] { benchmark->snapshot(); });
}

double
do_snapshot_ongoing_puts(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill();

  std::atomic<bool> done{};

  auto [_, snapshot_future] = benchmark->sync_and_snapshot();
  std::thread bg{[&benchmark, &done]() {
    while (!done)
      benchmark->write_items();
  }};

  double r = measure([&snapshot_future] {
    DBStatus<void> r = snapshot_future.get();
    if (unlikely(!r))
      fail(r.error());
  });

  done = true;
  bg.join();

  return r;
}

// do_blocks models a block loop: put num items on all available cores, then
// sync, rounds times. It prints each sync's wall time and overlapping phase
// timings, then the median of each column. Restrict cores with taskset.
void
do_blocks(int num, int value_size, int rounds)
{
  int threads = static_cast<int>(available_parallelism());
  std::unique_ptr<Benchmark> benchmark =
      make_benchmark(num, threads, value_size, SyncStatsMode::kEnabled);
  benchmark->prefill();

  std::println("# threads {}, write heads {}, items/block {}, value size {}",
               threads, scalable_shard_size(kBaseWriteHeadOrder), num,
               value_size);
  std::println("round, put_ms, sync_ms, drain_ms, inflight_ms, crc_ms, "
               "write_ms, flush_ms, fdatasync_ms, dirty_heads, "
               "early_mib, synced_mib, loglets_synced");

  constexpr size_t kCols = 12;
  std::vector<std::array<double, kCols>> rows;
  rows.reserve(static_cast<size_t>(rounds));
  auto ms = [](std::chrono::nanoseconds d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };
  auto mib = [](size_t n) { return static_cast<double>(n) / 1_MiB; };
  auto print_row = [](std::string_view label,
                      const std::array<double, kCols> &r) {
    std::print("{}", label);
    for (double v : r)
      std::print(", {:.3f}", v);
    std::println();
  };

  for (int i = 0; i < rounds; ++i) {
    double put_s = measure([&benchmark] { benchmark->write_items(); });
    benchmark->sync();
    SyncStats st = benchmark->last_sync_stats();
    rows.push_back({
        put_s * 1e3,
        ms(st.total),
        ms(st.drain),
        ms(st.inflight),
        ms(st.crc),
        ms(st.write),
        ms(st.flush),
        ms(st.fdatasync),
        static_cast<double>(st.dirty_heads),
        mib(st.bytes_early),
        mib(st.bytes_synced),
        static_cast<double>(st.loglets_synced),
    });
    print_row(std::to_string(i), rows.back());
  }

  std::array<double, kCols> median{};
  std::vector<double> col(rows.size());
  for (size_t c = 0; c < kCols; ++c) {
    for (size_t r = 0; r < rows.size(); ++r)
      col[r] = rows[r][c];
    std::ranges::sort(col);
    median[c] = std::midpoint(col[(col.size() - 1) / 2], col[col.size() / 2]);
  }
  print_row("median", median);
}

using bench_fn = double (*)(int num, int cores, int value_size);

void
run_bench(int num, int value_size, bench_fn fn)
{
  int cores = std::thread::hardware_concurrency();
  for (int i = 1; i <= cores; ++i) {
    double seconds = fn(num, i, value_size);
    std::println("{}, {:.0f}", i, num / seconds);
  }
}

const std::tuple<std::string, std::string, bench_fn> benchmarks[] = {
    {"fill", "inserts items", do_fill},
    {"replace", "replaces existing items", do_replacements},
    {"read", "reads items", do_reads},
    {"delete", "deletes items", do_deletions},
    {"reload", "reloads items from disk", do_reload},
    {"snapshot", "fills some, then snapshots", do_snapshot},
    {"snapshot_ongoing_puts",
     "fills some, then snapshots while puts are running in the background",
     do_snapshot_ongoing_puts},
};

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting db_bench!");

  auto print_usage = [] {
    std::println("usage: [db_path] [command] [num] [value_size] [rounds]");
    std::println("commands");
    for (const auto &[name, desc, ign] : hogd::benchmarks)
      std::println("  {}: {}", name, desc);
    std::println("  blocks: puts num items on all available cores then "
                 "syncs, [rounds] times (default 20); prints sync stats");
  };

  if (argc < 5) {
    print_usage();
    return 1;
  }
  hogd::db_path = argv[1];
  std::string cmd = argv[2];
  int num = std::stoi(argv[3], nullptr, 0);
  int value_size = std::stoi(argv[4], nullptr, 0);

  if (cmd == "blocks") {
    int rounds = argc > 5 ? std::stoi(argv[5], nullptr, 0) : 20;
    // each put thread writes num / threads items, so num must divide evenly.
    int threads = static_cast<int>(hogd::available_parallelism());
    if (num <= 0 || num % threads != 0 || rounds <= 0) {
      std::println("blocks: num must be a positive multiple of {} (the "
                   "available cores), rounds must be positive",
                   threads);
      return 1;
    }
    hogd::do_blocks(num, value_size, rounds);
    return 0;
  }

  for (const auto &[name, ign, fn] : hogd::benchmarks) {
    if (cmd != name)
      continue;
    hogd::run_bench(num, value_size, fn);
    return 0;
  }

  print_usage();
  return 1;
}
