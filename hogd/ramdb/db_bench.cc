// db_bench.cc - benchmarks for the database subsystem

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <numeric>
#include <print>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/resource.h>

#include "hogd/base/compiler.h"
#include "hogd/base/io.h"
#include "hogd/base/log.h"
#include "hogd/base/span.h"
#include "hogd/base/status.h"
#include "hogd/base/types.h"
#include "hogd/base/zipf.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/ramdb/db_status.h"

namespace hogd {

namespace {

std::string db_path;

// the Zipf skew used by the popularity benchmarks. 0.99 matches the skew used
// throughout the ObjecTier paper; 0 degenerates to uniform, which is the left
// edge of its skew sweep.
double bench_theta = 0.99;

// when positive, run at exactly this thread count instead of sweeping cores.
int bench_threads = 0;

// accesses performed per item held. The paper's Figure 2 uses 5e9 accesses over
// a 256 GB array of 128 B objects, i.e. 2.5; the touched-byte metric is
// sensitive to this ratio, so it has to be matched to compare against it.
int bench_access_mult = 1;

// item-space provisioning relative to the live set, for the churn benchmarks.
// Zero means use the built-in default.
double bench_heap_factor = 0.0;

// fixed seed so a consolidated run and a scattered run see the same layout
// shuffle and the same sequence of reads.
constexpr uint64_t kBenchSeed = 0x5DEECE66DULL;

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

// make_rank_key maps a popularity rank to a key; rank 0 is the hottest item.
// The rank-to-key mapping is fixed, so the consolidated and scattered layouts
// hold exactly the same key set and differ only in insertion order.
constexpr uint64_t
make_rank_key(uint32_t rank)
{
  return make_key(static_cast<int>(rank), 0);
}

// LatencyModel holds the two-tier access latencies used to model AMAT, in
// nanoseconds. The defaults match the ObjecTier paper: a DRAM fast tier and an
// Optane-class slow tier. Modelling rather than measuring is the paper's own
// Figure 2b methodology, and it needs no tiering hardware.
struct LatencyModel {
  double fast_ns;
  double slow_ns;
};
constexpr LatencyModel kLatencyModel{.fast_ns = 70.0, .slow_ns = 300.0};

// the share of occupied loglets that the fast tier is assumed to hold, matching
// the paper's top-20%-of-pages cutoff.
constexpr double kFastTierFraction = 0.20;

// the number of points emitted for each cumulative distribution.
constexpr size_t kCdfPoints = 100;

// default item-space provisioning for the churn benchmarks, relative to the
// live set. Overridable from the command line: how much slack the heap has is
// what decides whether the compactor runs at all, so it is an experimental
// variable rather than a constant.
constexpr double kChurnHeapFactor = 1.5;

// Layout selects the order in which items are inserted, which for a
// log-structured allocator is also the order they are packed into loglets.
enum class Layout : uint8_t {
  // insert in popularity order, so each loglet holds a contiguous run of
  // like-popularity items. This is the pre-arranged upper bound: the layout a
  // perfect consolidator would produce, at zero relocation cost.
  kConsolidated,
  // insert in an order uncorrelated with popularity, modelling the
  // popularity-oblivious layout real applications end up with.
  kScattered,
};

// build_permutation returns a fixed shuffle of [0, count).
std::vector<uint32_t>
build_permutation(int count, uint64_t seed)
{
  std::vector<uint32_t> perm(static_cast<size_t>(count));
  std::iota(perm.begin(), perm.end(), 0U);
  std::mt19937_64 rng(seed);
  std::ranges::shuffle(perm, rng);
  return perm;
}

// build_insertion_order returns the ranks to insert, in insertion order.
std::vector<uint32_t>
build_insertion_order(int count, Layout layout)
{
  if (layout == Layout::kScattered)
    return build_permutation(count, kBenchSeed);

  std::vector<uint32_t> order(static_cast<size_t>(count));
  std::iota(order.begin(), order.end(), 0U);
  return order;
}

// WriteHotness controls whether the churn stream's hot set coincides with the
// read stream's.
enum class WriteHotness : uint8_t {
  // Overwrites target the same keys that reads target. This is the easy case
  // for a log-structured store: hot keys are continuously rewritten to the
  // current write head, so write recency is a free and accurate hotness
  // signal, and loglets segregate by temperature without anyone trying.
  kCorrelated,
  // Overwrites target keys unrelated to the ones reads target, via a fixed
  // shuffle seeded differently from the layout shuffle. Write recency now says
  // nothing about read hotness, which is the case a read-hotness classifier
  // exists to handle.
  kIndependent,
};

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
  // read_items_zipf reads kv-pairs by popularity rank, drawn from a Zipf
  // distribution over the whole key space (not a per-thread slice, since the
  // skew being modelled is global).
  void read_items_zipf();
  // prefill_ranked initializes the database with rank-keyed items inserted in
  // the order the layout dictates.
  void prefill_ranked(Layout layout);
  // track_accesses makes read_items_zipf count accesses per rank, which
  // report_layout needs to classify items and weight the fast-tier hit rate.
  void track_accesses();
  // churn_items overwrites Zipf-selected items until the log has been forced
  // to recycle, which is what makes the compactor run at all.
  void churn_items(int rounds, WriteHotness hotness);
  // report_compaction prints what the compactor did during the run.
  void report_compaction(std::string_view label);
  // consolidate_by_access runs a consolidation pass classifying by the access
  // counts this benchmark recorded. An oracle, as elsewhere: it bounds what the
  // relocation mechanism can achieve independently of classifier quality.
  void consolidate_by_access();
  // report_faults replays the scored access sequence read-only and reports the
  // page faults it took. With the heap file-backed and the process capped, the
  // major count is a direct measurement of slow-tier accesses.
  void report_faults(std::string_view label);
  // report_layout joins the recorded access counts against each item's loglet
  // to produce the per-loglet utilization distributions and a modelled AMAT.
  void report_layout(std::string_view label);
  // delete_items deletes all the kv-pairs in the benchmark.
  void delete_items();
  // sync flushes and synchronizes all changes to disk.
  uint64_t sync();
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

  // per-rank access counts, indexed by rank. Empty unless track_accesses() has
  // been called. Relaxed atomics: workers only ever increment their own draws
  // and nothing reads the counts until every worker has joined.
  std::vector<std::atomic<uint32_t>> access_counts_;

  // item_count is the number of items actually written, which is a multiple of
  // threads_ because each worker takes an equal slice. Reads must draw from
  // this range rather than num_ so a truncated remainder is never looked up.
  int item_count() const { return (num_ / threads_) * threads_; }

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
Benchmark::read_items_zipf()
{
  Zipf zipf(static_cast<uint64_t>(item_count()), bench_theta);
  bool tracking = !access_counts_.empty();
  invoke_on_threads([this, &zipf, tracking](int thread_id, int samples) {
    uint64_t rng = kBenchSeed + static_cast<uint64_t>(thread_id);
    samples *= bench_access_mult;
    for (int i = 0; i < samples; ++i) {
      uint32_t rank = zipf(rng);
      uint64_t key = make_rank_key(rank);
      DBStatus<DBItem> ret = db_->get(byte_view(key));
      if (unlikely(!ret))
        fail(ret.error());
      if (tracking)
        access_counts_[rank].fetch_add(1, std::memory_order_relaxed);
    }
  });
}

void
Benchmark::track_accesses()
{
  access_counts_ =
      std::vector<std::atomic<uint32_t>>(static_cast<size_t>(item_count()));
}

void
Benchmark::churn_items(int rounds, WriteHotness hotness)
{
  // Each overwrite alternates the value size. A same-size put updates in place
  // (scalable_map.cc) -- it allocates nothing and tombstones nothing, so it
  // would never pressure memory and the compactor would never wake. Changing
  // the size takes the unlink-and-reinsert path instead: fresh log space
  // consumed, a tombstone left behind. That is what drives fragmentation up
  // and free loglets down to kLowMemoryRatio.
  //
  // Overwriting (rather than remove-then-put) keeps every key continuously
  // present, so concurrent readers never observe a miss.
  Zipf zipf(static_cast<uint64_t>(item_count()), bench_theta);

  // For kIndependent, the write stream draws from its own rank space and maps
  // through a shuffle seeded apart from the layout's, so the most-overwritten
  // keys bear no relation to the most-read ones.
  std::vector<uint32_t> write_map;
  if (hotness == WriteHotness::kIndependent)
    write_map = build_permutation(item_count(), kBenchSeed ^ 0xC0FFEEULL);

  invoke_on_threads([this, &zipf, &write_map, rounds](int thread_id,
                                                     int samples) {
    std::vector<std::byte> value(static_cast<size_t>(value_size_) +
                                 alignof(ItemHeader));
    uint64_t rng = kBenchSeed + 0x9E37U + static_cast<uint64_t>(thread_id);
    for (int round = 0; round < rounds; ++round) {
      size_t len = static_cast<size_t>(value_size_) +
                   ((round % 2 == 0) ? alignof(ItemHeader) : 0);
      for (int i = 0; i < samples; ++i) {
        uint32_t rank = zipf(rng);
        if (!write_map.empty())
          rank = write_map[rank];
        uint64_t key = make_rank_key(rank);
        DBStatus<void> ret =
            db_->put(byte_view(key), 0, std::span(value).first(len));
        if (unlikely(!ret))
          fail(ret.error());
      }
    }
  });
  sync();
}

void
Benchmark::report_compaction(std::string_view label)
{
  std::println("compaction,{},{},{}", label, db_->compaction_passes(),
               db_->compaction_bytes_reclaimed());
}

void
Benchmark::prefill_ranked(Layout layout)
{
  std::vector<uint32_t> order = build_insertion_order(item_count(), layout);
  invoke_on_threads([this, &order](int thread_id, int samples) {
    std::vector<std::byte> value(value_size_);
    size_t begin = static_cast<size_t>(thread_id) * static_cast<size_t>(samples);
    for (int i = 0; i < samples; ++i) {
      uint64_t key = make_rank_key(order[begin + static_cast<size_t>(i)]);
      DBStatus<void> ret = db_->put(byte_view(key), 0, std::span(value));
      if (unlikely(!ret))
        fail(ret.error());
    }
  });
  sync();
}

// emit_cdf prints a cumulative distribution of vals as kCdfPoints quantiles.
// vals is sorted in place.
void
emit_cdf(std::string_view label, std::string_view series,
         std::vector<double> &vals)
{
  if (vals.empty())
    return;
  std::ranges::sort(vals);
  for (size_t i = 0; i <= kCdfPoints; ++i) {
    size_t idx = (i * (vals.size() - 1)) / kCdfPoints;
    std::println("layout_cdf,{},{},{:.4f},{:.4f}", label, series,
                 static_cast<double>(i) / kCdfPoints, vals[idx]);
  }
}

void
Benchmark::consolidate_by_access()
{
  // Keys are the rank itself, so the access count is a direct lookup.
  auto is_hot = [this](ItemHeader *item) {
    uint64_t rank = 0;
    if (item->get_key().size() != sizeof(rank))
      return false;
    std::memcpy(&rank, item->get_key().data(), sizeof(rank));
    return rank < access_counts_.size() &&
           access_counts_[rank].load(std::memory_order_relaxed) >= 2;
  };
  size_t moved = db_->consolidate(is_hot, 0.0F);
  std::println("consolidated,{}", moved);
}

void
Benchmark::report_faults(std::string_view label)
{
  // Start both layouts from the same cold state. Without this the consolidated
  // arm is measured on the residue of the consolidation pass, which has just
  // streamed the whole heap through the resident-set cap.
  db_->evict_heap();

  struct rusage before{};
  struct rusage after{};
  getrusage(RUSAGE_SELF, &before);

  // Bounded replay. What we need is the fault *rate* per access; reading every
  // item once against a paging heap costs hours and measures nothing extra.
  constexpr int kMaxReplayAccesses = 500'000;
  int accesses = std::min(item_count(), kMaxReplayAccesses);

  Zipf zipf(static_cast<uint64_t>(item_count()), bench_theta);
  uint64_t rng = kBenchSeed + 0xFA17U;
  double seconds = measure([&] {
    for (int i = 0; i < accesses; ++i) {
      uint64_t key = make_rank_key(zipf(rng));
      DBStatus<DBItem> ret = db_->get(byte_view(key));
      if (unlikely(!ret))
        fail(ret.error());
    }
  });

  getrusage(RUSAGE_SELF, &after);
  std::println("faults,{},{},{},{},{:.3f}", label, accesses,
               after.ru_majflt - before.ru_majflt,
               after.ru_minflt - before.ru_minflt, seconds);
}

void
Benchmark::report_layout(std::string_view label)
{
  const size_t nloglets = db_->loglet_count();
  std::vector<size_t> touched_bytes(nloglets);
  std::vector<size_t> live_bytes(nloglets);
  std::vector<uint64_t> accesses(nloglets);
  size_t items = 0;

  // Join each live item's location against its recorded access count. This is
  // the paper's Figure 2a construction: a byte counts toward a tiering unit's
  // utilization if it was accessed at least once, and the denominator is the
  // whole unit, not just its live bytes.
  db_->for_each_location(
      [&](std::span<const std::byte> key, size_t bytes, size_t idx) {
        uint64_t rank = 0;
        if (key.size() != sizeof(rank))
          return;
        std::memcpy(&rank, key.data(), sizeof(rank));
        if (rank >= access_counts_.size())
          return;

        uint32_t n = access_counts_[rank].load(std::memory_order_relaxed);
        ++items;
        live_bytes[idx] += bytes;
        accesses[idx] += n;
        if (n > 0)
          touched_bytes[idx] += bytes;
      });

  // Only loglets holding data are tiering candidates; the heap is deliberately
  // oversized, so counting empty loglets would dilute every distribution.
  std::vector<size_t> occupied;
  for (size_t i = 0; i < nloglets; ++i)
    if (live_bytes[i] > 0)
      occupied.push_back(i);
  if (occupied.empty()) {
    std::println("layout_summary,{},0,0,0,0,0", label);
    return;
  }

  constexpr auto kUnit = static_cast<double>(kLogletRegionSize);
  std::vector<double> hot_util;
  std::vector<double> live_util;
  hot_util.reserve(occupied.size());
  live_util.reserve(occupied.size());
  uint64_t total_accesses = 0;
  for (size_t i : occupied) {
    hot_util.push_back(static_cast<double>(touched_bytes[i]) / kUnit);
    live_util.push_back(static_cast<double>(live_bytes[i]) / kUnit);
    total_accesses += accesses[i];
  }

  // Model the fast tier as the hottest kFastTierFraction of occupied loglets,
  // the way the paper's oracle picks the top 20% of pages.
  std::vector<size_t> by_heat = occupied;
  std::ranges::sort(by_heat, [&accesses](size_t a, size_t b) {
    return accesses[a] > accesses[b];
  });
  auto fast_count = static_cast<size_t>(
      std::ceil(kFastTierFraction * static_cast<double>(by_heat.size())));
  uint64_t fast_accesses = 0;
  double fast_hot_util = 0.0;
  for (size_t i = 0; i < fast_count; ++i) {
    fast_accesses += accesses[by_heat[i]];
    fast_hot_util += static_cast<double>(touched_bytes[by_heat[i]]) / kUnit;
  }
  // The paper's headline claim is about the fast tier specifically -- "even the
  // hottest pages in the fast tier can contain 50% cold data" -- so report the
  // hot-byte utilization of the tier, not just the mean over all loglets.
  fast_hot_util /= static_cast<double>(fast_count);

  double hit = total_accesses > 0 ? static_cast<double>(fast_accesses) /
                                        static_cast<double>(total_accesses)
                                  : 0.0;
  double amat = (hit * kLatencyModel.fast_ns) +
                ((1.0 - hit) * kLatencyModel.slow_ns);

  double mean_hot = std::accumulate(hot_util.begin(), hot_util.end(), 0.0) /
                    static_cast<double>(hot_util.size());
  double mean_live = std::accumulate(live_util.begin(), live_util.end(), 0.0) /
                     static_cast<double>(live_util.size());

  std::println("layout_summary,{},{},{},{},{:.4f},{:.4f},{:.4f}", label,
               occupied.size(), items, total_accesses, mean_hot, mean_live,
               fast_hot_util);
  std::println("layout_amat,{},{:.2f},{},{:.4f},{:.2f}", label,
               kFastTierFraction, fast_count, hit, amat);
  emit_cdf(label, "hot", hot_util);
  emit_cdf(label, "live", live_util);
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

// heap_factor sets how much item space is provisioned relative to the live set.
// The default 2.0 leaves ~50% of loglets free, which is far above
// kLowMemoryRatio, so the compactor never wakes -- fine for layout-only
// experiments, useless for anything that means to exercise compaction.
std::unique_ptr<Benchmark>
make_benchmark(size_t n, int threads, size_t value_size,
               double heap_factor = 2.0)
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

  // calculate a large enough heap size (goal: target ~half full by default).
  size_t heap_size = Database::estimate_heap_size(
      static_cast<size_t>(static_cast<double>(n) * heap_factor),
      sizeof(uint64_t), value_size);

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

// do_read_zipf_consolidated and do_read_zipf_scattered are the static-layout
// A/B: identical key sets and identical read streams, differing only in
// whether like-popularity items were packed together at insert time. Neither
// runs a consolidator, so the gap between them is the ceiling available to one
// — the ObjecTier paper's Figure 5 measured this way.
double
do_read_zipf_consolidated(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill_ranked(Layout::kConsolidated);
  return measure([&benchmark] { benchmark->read_items_zipf(); });
}

double
do_read_zipf_scattered(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill_ranked(Layout::kScattered);
  return measure([&benchmark] { benchmark->read_items_zipf(); });
}

// do_ssd_tier is Phase 3a: the same scattered-vs-consolidated comparison, but
// with the heap file-backed and the process memory-capped so page faults are a
// real, measured count of slow-tier accesses rather than a modelled AMAT.
//
// Run through ramdb_bench rather than hogdb_bench deliberately: hogdb's
// per-block item_locks_ and ops vector make its *anonymous* footprint larger
// than its heap working set, so a memory cap OOM-kills the process before it can
// evict any heap pages. Here the heap dominates, which is what the cap needs.
double
do_ssd_tier(int num, int cores, int value_size)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill_ranked(Layout::kScattered);
  benchmark->track_accesses();
  benchmark->read_items_zipf();
  benchmark->report_layout("scattered");
  benchmark->report_faults("scattered");

  benchmark->consolidate_by_access();
  benchmark->report_layout("consolidated");
  benchmark->report_faults("consolidated");
  return 0.0;
}

// do_layout_* are X1: after a skewed read workload, report how much of each
// loglet is data that was actually accessed. The consolidated run is the
// oracle-sorted upper bound; the scattered run is the layout a
// popularity-oblivious allocator produces. Their gap is the paper's Figure 2a
// on a real store, and the modelled AMAT is its Figure 2b.
double
do_layout(int num, int cores, int value_size, Layout layout,
          std::string_view label)
{
  std::unique_ptr<Benchmark> benchmark = make_benchmark(num, cores, value_size);
  benchmark->prefill_ranked(layout);
  benchmark->track_accesses();
  double seconds = measure([&benchmark] { benchmark->read_items_zipf(); });
  benchmark->report_layout(label);
  return seconds;
}

// do_churn_* are H5: prefill a known layout, then overwrite enough to force the
// compactor to run, and only then measure the layout. Every experiment before
// this one was prefill-only, so compaction never fired and no claim about the
// compactor was actually being tested. The question this answers is whether
// liveness-driven relocation destroys popularity locality that was there.
double
do_churn(int num, int cores, int value_size, Layout layout,
         WriteHotness hotness, std::string_view label)
{
  // A tighter heap than the other benchmarks use: with 2x provisioning, free
  // loglets bottom out around 30% and kLowMemoryRatio (10%) is never reached,
  // so no amount of churn wakes the compactor. Real deployments do not run at
  // 50% headroom either.
  double factor =
      bench_heap_factor > 0.0 ? bench_heap_factor : kChurnHeapFactor;
  std::unique_ptr<Benchmark> benchmark =
      make_benchmark(num, cores, value_size, factor);
  benchmark->prefill_ranked(layout);
  double seconds = measure([&benchmark, hotness] {
    benchmark->churn_items(bench_access_mult, hotness);
  });

  // Measure the layout the compactor left behind, not the one prefill built.
  benchmark->track_accesses();
  benchmark->read_items_zipf();
  benchmark->report_layout(label);
  benchmark->report_compaction(label);
  return seconds;
}

double
do_churn_consolidated(int num, int cores, int value_size)
{
  return do_churn(num, cores, value_size, Layout::kConsolidated,
                  WriteHotness::kCorrelated, "churn_consolidated");
}

double
do_churn_scattered(int num, int cores, int value_size)
{
  return do_churn(num, cores, value_size, Layout::kScattered,
                  WriteHotness::kCorrelated, "churn_scattered");
}

// do_churn_decorrelated is the control for the H5 finding. churn_scattered
// draws its overwrites from the same Zipf as its reads, so write recency is a
// near-perfect hotness proxy and the store segregates by temperature for free.
// This run changes only that one thing, so the difference between the two is
// how much of ramdb's implicit consolidation was an artifact of the workload.
double
do_churn_decorrelated(int num, int cores, int value_size)
{
  return do_churn(num, cores, value_size, Layout::kScattered,
                  WriteHotness::kIndependent, "churn_decorrelated");
}

double
do_layout_consolidated(int num, int cores, int value_size)
{
  return do_layout(num, cores, value_size, Layout::kConsolidated,
                   "consolidated");
}

double
do_layout_scattered(int num, int cores, int value_size)
{
  return do_layout(num, cores, value_size, Layout::kScattered, "scattered");
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

using bench_fn = double (*)(int num, int cores, int value_size);

void
run_bench(int num, int value_size, bench_fn fn)
{
  // A fixed thread count runs a single point instead of the scaling sweep.
  // Sweeping cores is wasted work when the independent variable is something
  // else (skew, layout, item size) and the machine has many cores.
  if (bench_threads > 0) {
    double seconds = fn(num, bench_threads, value_size);
    std::println("{}, {:.0f}", bench_threads, num / seconds);
    return;
  }

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
    {"read_zipf_consolidated",
     "skewed reads over a popularity-ordered layout", do_read_zipf_consolidated},
    {"read_zipf_scattered",
     "skewed reads over a popularity-oblivious layout", do_read_zipf_scattered},
    {"ssd_tier",
     "scattered vs consolidated page-fault counts against a file-backed heap",
     do_ssd_tier},
    {"layout_consolidated",
     "per-loglet utilization CDF over a popularity-ordered layout",
     do_layout_consolidated},
    {"layout_scattered",
     "per-loglet utilization CDF over a popularity-oblivious layout",
     do_layout_scattered},
    {"churn_consolidated",
     "churns a popularity-ordered layout until the compactor runs, then "
     "reports the layout it left",
     do_churn_consolidated},
    {"churn_scattered",
     "churns a popularity-oblivious layout until the compactor runs, then "
     "reports the layout it left",
     do_churn_scattered},
    {"churn_decorrelated",
     "churns keys unrelated to the ones being read, so write recency is not a "
     "hotness signal",
     do_churn_decorrelated},
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
    std::println("usage: [db_path] [command] [num] [value_size] [theta] "
                 "[threads] [mult] [heap]");
    std::println("theta:   Zipf skew for the read_zipf_* commands "
                 "(default 0.99; 0 is uniform)");
    std::println("threads: fixed thread count; omit to sweep 1..cores");
    std::println("mult:    work per item -- accesses for read_zipf_/layout_ "
                 "(the paper's Fig 2 uses 2.5), overwrite rounds for churn_ "
                 "(default 1)");
    std::println("heap:    churn_ item-space provisioning vs the live set "
                 "(default 1.5; 2.0 never reaches kLowMemoryRatio)");
    std::println("commands");
    for (const auto &[name, desc, ign] : hogd::benchmarks)
      std::println("  {}: {}", name, desc);
  };

  if (argc < 5) {
    print_usage();
    return 1;
  }
  hogd::db_path = argv[1];
  std::string cmd = argv[2];
  int num = std::stoi(argv[3], nullptr, 0);
  int value_size = std::stoi(argv[4], nullptr, 0);
  if (argc > 5)
    hogd::bench_theta = std::stod(argv[5]);
  if (argc > 6)
    hogd::bench_threads = std::stoi(argv[6], nullptr, 0);
  if (argc > 7)
    hogd::bench_access_mult = std::stoi(argv[7], nullptr, 0);
  if (argc > 8)
    hogd::bench_heap_factor = std::stod(argv[8]);

  for (const auto &[name, ign, fn] : hogd::benchmarks) {
    if (cmd != name)
      continue;
    hogd::run_bench(num, value_size, fn);
    return 0;
  }

  print_usage();
  return 1;
}
