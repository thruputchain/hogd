// db_bench.cc - benchmarks for the database subsystem

#include <bit>
#include <chrono>
#include <cstddef>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

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
  void sync();
  // initializes the database with items (initial state for benchmarks).
  void prefill();

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

void
Benchmark::sync()
{
  DBStatus<uint64_t> ret = db_->sync();
  if (unlikely(!ret))
    fail(ret.error());
}

void
Benchmark::prefill()
{
  write_items();
  sync();
}

std::unique_ptr<Benchmark>
make_benchmark(size_t n, int threads, size_t value_size)
{
  // calculate a large enough heap size (goal: target ~half full).
  size_t heap_size =
      Database::estimate_heap_size(n * 2, sizeof(uint64_t), value_size);

  // calculate the bucket order (goal: load factor of 1-2).
  int bucket_order = std::countr_zero(std::bit_floor(n));

  // create the database.
  DBStatus<std::unique_ptr<Database>> db =
      Database::open(db_path, heap_size, bucket_order);
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

const std::pair<std::string, bench_fn> benchmarks[] = {
    {"fill", do_fill},
    {"replace", do_replacements},
    {"read", do_reads},
    {"delete", do_deletions},
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
    std::println("usage: [db_path] [command] [num] [value_size]");
    std::println("commands");
    std::println("  fill: inserts items");
    std::println("  replace: replaces existing items");
    std::println("  read: reads items");
    std::println("  delete: deletes items");
    std::println("  compact: reads items while compacting");
  };

  if (argc < 5) {
    print_usage();
    return 1;
  }
  hogd::db_path = argv[1];
  std::string cmd = argv[2];
  int num = std::stoi(argv[3], nullptr, 0);
  int value_size = std::stoi(argv[4], nullptr, 0);

  for (const auto &[name, fn] : hogd::benchmarks) {
    if (cmd != name)
      continue;
    hogd::run_bench(num, value_size, fn);
    return 0;
  }

  print_usage();
  return 1;
}
