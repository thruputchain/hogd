// hogdb_bench.cc - benchmarks for hogdb

#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <print>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <getopt.h> // NOLINT(misc-include-cleaner)

#include "consensus.hh"
#include "header.hh"
#include "hogd/base/bits.h"
#include "hogd/base/compiler.h"
#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/status.h"
#include "hogd/base/zipf.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

constexpr int128_t kDefaultBalance = 100;

// config parameter: location to store ramdb.
std::string db_path;

// Zipf skew for zipf_payments. 0.99 matches the ObjecTier paper; 0 is uniform,
// which makes zipf_payments equivalent to a random-payee workload.
double bench_theta = 0.99;

// Multiplier on the provisioned heap. The other payment benchmarks inflate the
// heap so the compactor never runs, which is right for measuring execution
// throughput and wrong for measuring layout -- compaction is what relocates
// items. Lower this to let the compactor run.
double bench_heap_factor = 8.0;

// fixed seed so repeated runs generate the same payee distribution.
constexpr uint64_t kBenchSeed = 0x5DEECE66DULL;

// when true, hot payees occupy contiguous payee slots: the oracle-consolidated
// layout, which bounds what any consolidator could achieve.
bool bench_sorted_hot = false;

// Two-tier latency model and fast-tier share, matching ramdb_bench so the
// numbers from the two binaries are comparable.
constexpr double kFastNs = 70.0;
constexpr double kSlowNs = 300.0;
constexpr double kFastTierFraction = 0.20;

void
fail(HogDBError err)
{
  panic("Unexpected error: {}", err);
}

// Returns the number of buckets of size `bucket_size` needed to hold `n` items.
constexpr size_t
bucket_count(size_t n, size_t bucket_size)
{
  return (n + bucket_size - 1) / bucket_size;
}

// measure returns the execution time of fn in seconds (as a double)
double
measure(auto &&fn)
{
  using clock = std::chrono::high_resolution_clock;
  clock::time_point start = clock::now();
  std::forward<decltype(fn)>(fn)();
  clock::time_point end = clock::now();
  return std::chrono::duration<double>(end - start).count();
}

struct Payment {
  ItemKey src;
  ItemKey dst;
  int128_t amount;
  std::optional<Uniquifier> uniquifier;
};

class Benchmark {
public:
  static std::unique_ptr<Benchmark>
  make_one_to_many_payments_benchmark(size_t n, int workers, bool use_nonces,
                                      bool db_hash);
  static std::unique_ptr<Benchmark>
  make_many_to_one_payments_benchmark(size_t n, int workers, bool use_nonces,
                                      bool db_hash);
  static std::unique_ptr<Benchmark>
  make_round_robin_payments_benchmark(size_t n, int workers, bool use_nonces,
                                      bool db_hash);
  static std::unique_ptr<Benchmark>
  make_zipf_payments_benchmark(size_t n, int workers, bool use_nonces,
                               bool db_hash);
  static std::unique_ptr<Benchmark> make_new_storage_benchmark(size_t n,
                                                               int workers,
                                                               bool use_nonces,
                                                               bool db_hash);

  Benchmark(HogDBDAO db, int workers, bool compute_db_hash,
            std::vector<Payment> &&ops)
    : db_(std::move(db)), workers_(workers), ops_(std::move(ops)),
      compute_db_hash_(compute_db_hash)
  {}
  ~Benchmark() = default;

  // run executes the benchmark. This function can be timed directly to measure
  // performance.
  void run();

  // report_layout joins the payee access counts this benchmark generated
  // against the loglet each item actually landed in, and reports the resulting
  // fast-tier occupancy and modelled AMAT. Only meaningful for a skewed
  // command; a uniform one has no hot set to consolidate.
  void report_layout(std::string_view label);

private:
  HogDBDAO db_;
  WorkerPool workers_;
  std::vector<Payment> ops_;
  // Access count per payee, keyed by the exact ramdb key bytes hogdb stores the
  // item under. Built by the skewed builder via the same make_contract_data_key
  // the DAO uses, so matching needs no assumptions about key layout. The
  // benchmark generates the workload, so it knows the distribution exactly and
  // needs no instrumentation on the read path.
  std::unordered_map<std::string, uint32_t> payee_hits_;
  AuthChangeSubscriptionManager subscription_manager_;
  bool compute_db_hash_;

  StorageCostModel cost_model_{StorageCostModel::default_cost_model()};
};

namespace {

constexpr ItemKey
make_key(size_t addr, size_t item)
{
  return make_item_key(make_address(addr), item);
}

// key_bytes_string renders a ramdb key as a lookup-friendly opaque string. Only
// used to match items against the generated access distribution.
inline std::string
key_bytes_string(std::span<const std::byte> key)
{
  return std::string(reinterpret_cast<const char *>(key.data()), key.size());
}

constexpr std::optional<Uniquifier>
maybe_uniquifier(BlockHeight height, uint64_t nonce, bool use_nonces)
{
  if (!use_nonces)
    return std::nullopt;
  return Uniquifier{.block_expiry_height = height, .nonce = nonce};
}

HogDBDAO
open_ramdb(size_t n)
{
  // estimate size based on 1 x nnint128 and 1 x account_balance per account
  size_t heap_size =
      Database::estimate_heap_size(n * 2, sizeof(ItemKey), sizeof(int128_t));
  // Inflated so the compactor stays idle by default; lower bench_heap_factor to
  // let it run (see the flag's declaration).
  heap_size = static_cast<size_t>(static_cast<double>(heap_size) *
                                  bench_heap_factor);

  int bucket_order = std::countr_zero(std::bit_floor(n));
  HogDBStatus<std::unique_ptr<Database>> db =
      Database::open(db_path, heap_size, bucket_order)
          .transform_error(to_hogdb_error);
  if (!db)
    fail(db.error());
  return HogDBDAO(RAMDB(std::move(*db)));
}

void
populate_account_balances(HogDBDAO &db, size_t n,
                          int128_t val = kDefaultBalance)
{
  for (size_t i = 0; i < n; i++) {
    if (auto ret = db.write_address_balance(make_address(i), val); !ret)
      fail(ret.error());
  }
}

} // namespace

std::unique_ptr<Benchmark>
Benchmark::make_one_to_many_payments_benchmark(size_t n, int workers,
                                               bool nonces, bool db_hash)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n - 1);
  for (size_t i = 1; i < n; i++) {
    // db tip at start is height 1, so all uniquifiers must have
    // expiry height of at least 1.
    ops.emplace_back(make_key(0, 0), make_key(0, i), 1,
                     maybe_uniquifier((i % 1024) + 1, i, nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), workers, db_hash,
                                           std::move(ops));

  // set up initial DB state.
  auto ret =
      bench->db_.write_nnint128(make_key(0, 0), static_cast<int128_t>(n));
  if (!ret)
    fail(ret.error());
  for (size_t i = 1; i < n; i++) {
    ret = bench->db_.write_nnint128(make_key(0, i), 0);
    if (!ret)
      fail(ret.error());
  }

  // one account acts as source. if each has a uniquifier and they all get
  // committed as part of a single block, then the source account needs balance
  // to cover the storage cost of a replay entry for every transaction.
  populate_account_balances(bench->db_, n,
                            (bench->cost_model_.kBaseItemCost +
                             bench->cost_model_.kReplayCacheCostPerEntry) *
                                n);
  auto lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_many_to_one_payments_benchmark(size_t n, int workers,
                                               bool nonces, bool db_hash)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n - 1);
  for (size_t i = 1; i < n; i++) {
    ops.emplace_back(make_key(i, 0), make_key(0, 0), 1,
                     maybe_uniquifier(1, i, nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), workers, db_hash,
                                           std::move(ops));

  // set up initial DB state.
  auto ret =
      bench->db_.write_nnint128(make_key(0, 0), static_cast<int128_t>(0));
  if (!ret)
    fail(ret.error());
  for (size_t i = 1; i < n; i++) {
    ret = bench->db_.write_nnint128(make_key(i, 0), 1);
    if (!ret)
      fail(ret.error());
  }
  // initial storage amount per account should be sufficient for replay cache
  // entries because each source only needs to record a single entry.
  populate_account_balances(bench->db_, n,
                            bench->cost_model_.kBaseItemCost +
                                bench->cost_model_.kReplayCacheCostPerEntry);
  auto lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_zipf_payments_benchmark(size_t n, int workers, bool nonces,
                                       bool db_hash)
{
  // Everyone pays a Zipf-selected payee. This is manytoone_payments generalized
  // off its degenerate endpoint: that mode sends every payment to a single
  // account, which is a hot set of one item and therefore permanently
  // cache-resident and invisible to a memory-tiering experiment. Grading the
  // payee distribution produces a hot set with real byte extent -- large enough
  // to miss cache, small enough to fit a fast tier -- which is the regime
  // consolidation actually addresses.
  //
  // Each payer is still used exactly once, so replay-cache and balance setup
  // stay identical to manytoone_payments.
  std::vector<Payment> ops;
  ops.reserve(n - 1);
  std::vector<uint32_t> hits(n, 0);
  Zipf zipf(n, bench_theta);
  uint64_t rng = kBenchSeed;
  // Payee items are created in index order during setup, so a Zipf rank used
  // directly as an index would make the hot set physically contiguous -- the
  // oracle-consolidated layout, by accident. Map rank through a fixed shuffle so
  // hot payees are scattered across the log the way a popularity-oblivious
  // allocator would leave them. Pass --sorted to measure the ceiling instead.
  std::vector<uint32_t> slot(n);
  std::iota(slot.begin(), slot.end(), 0U);
  if (!bench_sorted_hot) {
    std::mt19937_64 shuffle_rng(kBenchSeed ^ 0xA5A5A5ULL);
    std::ranges::shuffle(slot, shuffle_rng);
  }
  for (size_t i = 1; i < n; i++) {
    uint32_t payee = slot[zipf(rng)];
    hits[payee]++;
    ops.emplace_back(make_key(i, 0), make_key(0, payee), 1,
                     maybe_uniquifier(1, i, nonces));
  }

  auto bench = std::make_unique<Benchmark>(open_ramdb(n), workers, db_hash,
                                           std::move(ops));
  // Record hits against the exact key bytes hogdb will store each payee under.
  for (size_t i = 0; i < n; i++) {
    if (hits[i] == 0)
      continue;
    auto db_key = make_contract_data_key(make_key(0, i));
    bench->payee_hits_.emplace(key_bytes_string(std::as_bytes(std::span(db_key))),
                               hits[i]);
  }

  // set up initial DB state: one nnint128 per payee item, one per payer.
  for (size_t i = 0; i < n; i++) {
    if (auto ret = bench->db_.write_nnint128(make_key(0, i), 0); !ret)
      fail(ret.error());
  }
  for (size_t i = 1; i < n; i++) {
    if (auto ret = bench->db_.write_nnint128(make_key(i, 0), 1); !ret)
      fail(ret.error());
  }
  // as in manytoone, each source records a single replay entry.
  populate_account_balances(bench->db_, n,
                            bench->cost_model_.kBaseItemCost +
                                bench->cost_model_.kReplayCacheCostPerEntry);
  auto lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_round_robin_payments_benchmark(size_t n, int workers,
                                               bool use_nonces, bool db_hash)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ops.emplace_back(make_key(i, 0), make_key((i + 1) % n, 0), 1,
                     maybe_uniquifier(1, i, use_nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), workers, db_hash,
                                           std::move(ops));

  // set up initial DB state.
  for (size_t i = 0; i < n; i++) {
    if (auto ret = bench->db_.write_nnint128(make_key(i, 0), 1); !ret)
      fail(ret.error());
  }
  // initial storage amount per account should be sufficient for replay cache
  // entries because each source only needs to record a single entry.
  populate_account_balances(bench->db_, n,
                            bench->cost_model_.kBaseItemCost +
                                bench->cost_model_.kReplayCacheCostPerEntry);
  if (auto lsn = bench->db_.commit(); !lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_new_storage_benchmark(size_t n, int workers, bool use_nonces,
                                      bool db_hash)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ops.emplace_back(make_key(i, 0), make_key(i, 1), 1,
                     maybe_uniquifier(1, i, use_nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), workers, db_hash,
                                           std::move(ops));

  // set up initial DB state.
  for (size_t i = 0; i < n; i++) {
    if (auto ret = bench->db_.write_nnint128(make_key(i, 0), 1); !ret)
      fail(ret.error());
  }
  // Each TX creates a new dst nnint128 (kBaseItemCost) and, with nonces, a
  // replay cache entry (kReplayCacheCostPerEntry). The balance must also cover
  // the existing src key's storage cost (kBaseItemCost) since the balance lock
  // charges the total absolute storage cost for the address.
  populate_account_balances(bench->db_, n,
                            (2 * bench->cost_model_.kBaseItemCost) +
                                bench->cost_model_.kReplayCacheCostPerEntry);
  if (auto lsn = bench->db_.commit(); !lsn)
    fail(lsn.error());

  return bench;
}

void
Benchmark::run()
{
  swap_logger(std::make_unique<ConsoleLogger>(LogLevel::kTrace));
  trace("trace logging enabled");

  BackgroundCommitWorker background_commit_worker(subscription_manager_,
                                                  nullptr);
  PerThreadARC<BlockTX> block(db_, cost_model_, background_commit_worker, 1,
                              BlockExecutionMode::kBlockExecutionModeProposal);

  ConsensusValue tip_value{};
  tip_value.proposal_header.block_height = 0;
  DbTip tip{.id = std::numeric_limits<BlockTXID>::max(),
            .storage_long_hash = {},
            .value = std::move(tip_value)};
  if (auto ret = db_.write_dbtip(tip); !ret)
    fail(ret.error());

  // invoke_tx simulates DB operations from a contract invocation.
  auto invoke_fn = [&block, &tip](const Payment &p) {
    CHECK(block.try_acquire_reference());

    InvokeTX tx(block);
    if (p.uniquifier) {
      CHECK_GE(p.uniquifier->block_expiry_height,
               tip.value.proposal_header.block_height);
      auto ret =
          tx.request_consume_uniquifier(p.src.get_address(), *p.uniquifier,
                                        std::to_underlying(
                                            StorageDeltaFlags::None));
      if (!ret)
        fail(ret.error());
    }
    if (auto ret =
            tx.request_nnint128(p.src, -p.amount,
                                std::to_underlying(StorageDeltaFlags::None));
        !ret)
      fail(ret.error());
    if (auto ret =
            tx.request_nnint128(p.dst, p.amount,
                                std::to_underlying(StorageDeltaFlags::None));
        !ret)
      fail(ret.error());
    if (auto ret = tx.sync_balance_locks(); !ret)
      fail(ret.error());
    if (auto ret = tx.commit(); !ret)
      fail(ret.error());
  };

  // run invoke_tx in parallel on the workers for all payment ops.
  constexpr size_t kBatchesPerWorker = 4;
  size_t width = ops_.size() / workers_.worker_count() / kBatchesPerWorker;
  workers_.for_each_parallel(ops_, invoke_fn, std::max(width, 1UL));

  // Close the block: stage changes and optionally compute the hash.
  if (compute_db_hash_) {
    auto hash_result = block.get().close(StorageLongHash{});
    CHECK(hash_result.has_value());
    asm volatile("" : : "m"(*hash_result) : "memory");
  }
  else {
    CHECK(block.get().close_skip_hash_for_testing().has_value());
  }

  // commit changes to the ramdb.
  auto ret = block.get().commit(workers_).and_then([this] {
    // persist everything to the database.
    return db_.commit().transform([](uint64_t lsn) {});
  });
  if (!ret)
    fail(ret.error());

  CHECK(block.initiate_shutdown());
}

void
Benchmark::report_layout(std::string_view label)
{
  if (payee_hits_.empty()) {
    warn("{}: no access distribution recorded; layout report skipped", label);
    return;
  }

  RAMDB &ramdb = db_.get_ramdb();
  const size_t nloglets = ramdb.loglet_count();
  std::vector<size_t> touched_bytes(nloglets);
  std::vector<size_t> live_bytes(nloglets);
  std::vector<uint64_t> accesses(nloglets);

  // Recover each payee item's index from its key so it can be matched against
  // the generated hit counts. Items that are not payees (payer balances, replay
  // entries) contribute live bytes but no accesses -- which is correct: they are
  // cold data competing for the same loglets.
  ramdb.for_each_location([&](std::span<const std::byte> key, size_t bytes,
                              size_t idx) {
    live_bytes[idx] += bytes;
    auto it = payee_hits_.find(key_bytes_string(key));
    if (it == payee_hits_.end())
      return;
    accesses[idx] += it->second;
    touched_bytes[idx] += bytes;
  });

  std::vector<size_t> occupied;
  for (size_t i = 0; i < nloglets; ++i)
    if (live_bytes[i] > 0)
      occupied.push_back(i);
  if (occupied.empty()) {
    warn("{}: no occupied loglets", label);
    return;
  }

  std::ranges::sort(occupied, [&accesses](size_t a, size_t b) {
    return accesses[a] > accesses[b];
  });
  auto fast_count = static_cast<size_t>(
      std::ceil(kFastTierFraction * static_cast<double>(occupied.size())));

  uint64_t total = 0;
  for (size_t i : occupied)
    total += accesses[i];
  uint64_t fast = 0;
  double fast_hot = 0.0;
  for (size_t i = 0; i < fast_count; ++i) {
    fast += accesses[occupied[i]];
    fast_hot += static_cast<double>(touched_bytes[occupied[i]]) /
                static_cast<double>(kLogletRegionSize);
  }
  fast_hot /= static_cast<double>(fast_count);

  double hit = total > 0 ? static_cast<double>(fast) /
                               static_cast<double>(total)
                         : 0.0;
  double amat = (hit * kFastNs) + ((1.0 - hit) * kSlowNs);

  std::println("layout,{},{},{},{:.4f},{:.4f},{:.2f}", label, occupied.size(),
               fast_count, fast_hot, hit, amat);
  std::println("compaction,{},{},{}", label, ramdb.compaction_passes(),
               ramdb.compaction_bytes_reclaimed());
}

using bench_fn = std::unique_ptr<Benchmark> (*)(size_t n, int workers,
                                                bool use_nonces, bool db_hash);

const std::tuple<std::string, std::string, bench_fn> benchmarks[] = {
    {"roundrobin_payments", "each payer pays exactly one other payer",
     Benchmark::make_round_robin_payments_benchmark},
    {"onetomany_payments", "one payer pays everyone else",
     Benchmark::make_one_to_many_payments_benchmark},
    {"manytoone_payments", "everyone pays one payee",
     Benchmark::make_many_to_one_payments_benchmark},
    {"zipf_payments", "everyone pays a Zipf-selected payee (graded hot set)",
     Benchmark::make_zipf_payments_benchmark},
    {"new_storage", "new nnint128's are stored on the ledger",
     Benchmark::make_new_storage_benchmark},
};

// Encode a 32-bit index as a 4-byte little-endian QueueEntry.
QueueEntry
make_rcq_entry(uint32_t index)
{
  QueueEntry entry;
  uint32_t le = cpu_to_le(index);
  auto bytes = std::as_bytes(std::span(&le, 1));
  for (auto b : bytes)
    entry.push_back(static_cast<uint8_t>(b));
  return entry;
}

class RCQBenchmark {
public:
  static std::unique_ptr<RCQBenchmark> make(size_t num_entries, int num_workers,
                                            size_t num_queues, bool concurrent,
                                            int num_producers,
                                            bool compute_db_hash);

  void run();

private:
  RCQBenchmark(HogDBDAO db, int num_workers, size_t num_entries,
               size_t num_queues, bool concurrent, int num_producers,
               bool compute_db_hash)
    : db_(std::move(db)), workers_(num_workers), num_entries_(num_entries),
      num_queues_(num_queues), concurrent_(concurrent),
      num_producers_(num_producers), compute_db_hash_(compute_db_hash)
  {}

  void run_sequential();
  void run_concurrent();

  void close_block(BlockTX &btx) const
  {
    if (compute_db_hash_) {
      CHECK(btx.close(StorageLongHash{}).has_value());
    }
    else {
      CHECK(btx.close_skip_hash_for_testing().has_value());
    }
  }

  HogDBDAO db_;
  WorkerPool workers_;
  StorageCostModel cost_model_{StorageCostModel::default_cost_model()};
  AuthChangeSubscriptionManager subscription_manager_;
  BackgroundCommitWorker background_commit_worker_{subscription_manager_,
                                                   nullptr};

  size_t num_entries_;
  size_t num_queues_;
  bool concurrent_;
  size_t num_producers_;
  bool compute_db_hash_;

  std::vector<ItemKey> queue_keys_;
};

std::unique_ptr<RCQBenchmark>
RCQBenchmark::make(size_t num_entries, int num_workers, size_t num_queues,
                   bool concurrent, int num_producers, bool compute_db_hash)
{
  // Estimate DB items for heap sizing: each queue needs its data blocks,
  // a metadata item, and a balance entry.
  size_t blocks_per_queue = bucket_count(num_entries, num_queues * 1000);
  size_t n = num_queues * (blocks_per_queue + 2);
  auto bench = std::unique_ptr<RCQBenchmark>(
      new RCQBenchmark(open_ramdb(n), num_workers, num_entries, num_queues,
                       concurrent, num_producers, compute_db_hash));

  // Generate queue keys: queue i lives at address i, item slot 0.
  bench->queue_keys_.reserve(num_queues);
  for (size_t i = 0; i < num_queues; i++)
    bench->queue_keys_.push_back(make_key(i, 0));

  // Fund each queue-owning address with generous balance for storage costs.
  // Worst case: all entries in one queue, plus blocks.
  int128_t balance_per_addr =
      (bench->cost_model_.kRCQCostPerEntry + bench->cost_model_.kBaseItemCost) *
      static_cast<int128_t>(num_entries + 1000);
  populate_account_balances(bench->db_, num_queues, balance_per_addr);

  // Write initial dbtip.
  ConsensusValue tip_value{};
  tip_value.proposal_header.block_height = 0;
  DbTip tip{.id = std::numeric_limits<BlockTXID>::max(),
            .storage_long_hash = {},
            .value = std::move(tip_value)};
  if (HogDBStatus<void> ret = bench->db_.write_dbtip(tip); !ret)
    fail(ret.error());

  if (HogDBStatus<uint64_t> lsn = bench->db_.commit(); !lsn)
    fail(lsn.error());

  return bench;
}

void
RCQBenchmark::run()
{
  swap_logger(std::make_unique<ConsoleLogger>(LogLevel::kTrace));
  trace("trace logging enabled");

  if (concurrent_)
    run_concurrent();
  else
    run_sequential();
}

void
RCQBenchmark::run_sequential()
{
  constexpr size_t kEntriesPerBlock = kRetrocausalQueueBlockEntriesMaxLen;
  size_t num_blocks =
      std::max(bucket_count(num_entries_, num_queues_ * kEntriesPerBlock), 1UL);

  // Phase 1: Insert
  size_t wb_before_insert = db_.disk_bytes_written();
  double insert_sec = measure([&] {
    size_t block_start_idx = 0;
    for (size_t block = 0; block < num_blocks; block++) {
      PerThreadARC<BlockTX> block_tx(
          db_, cost_model_, background_commit_worker_,
          static_cast<BlockHeight>(block + 1),
          BlockExecutionMode::kBlockExecutionModeProposal);

      size_t block_end =
          std::min(num_entries_,
                   block_start_idx + (num_queues_ * kEntriesPerBlock));

      std::vector<size_t> work_items;
      work_items.reserve(block_end - block_start_idx);
      for (size_t i = block_start_idx; i < block_end; i++)
        work_items.push_back(i);

      auto insert_fn = [&](size_t i) {
        CHECK(block_tx.try_acquire_reference());

        InvokeTX tx(block_tx);
        RetrocausalQueueDelta delta;
        delta.insert.added_entries.push_back(
            make_rcq_entry(static_cast<uint32_t>(i)));
        if (auto ret = tx.request_retrocausal_queue(
                queue_keys_[i % num_queues_], std::move(delta),
                std::to_underlying(StorageDeltaFlags::None));
            !ret)
          fail(ret.error());
        if (auto ret = tx.sync_balance_locks(); !ret)
          fail(ret.error());
        if (auto ret = tx.commit(); !ret)
          fail(ret.error());
      };

      constexpr size_t kBatchesPerWorker = 4;
      size_t width =
          work_items.size() / workers_.worker_count() / kBatchesPerWorker;
      workers_.for_each_parallel(work_items, insert_fn, std::max(width, 1UL));

      close_block(block_tx.get());
      HogDBStatus<void> ret = block_tx.get().commit(workers_).and_then(
          [this] { return db_.commit().transform([](uint64_t) {}); });
      if (!ret)
        fail(ret.error());
      info("committed block {}/{}", block, num_blocks);

      CHECK(block_tx.initiate_shutdown());

      block_start_idx = block_end;
    }
  });
  size_t wb_after_insert = db_.disk_bytes_written();
  std::println("insert: {} seconds ({} entries/sec)", insert_sec,
               num_entries_ / insert_sec);
  std::println("insert disk_bytes_written: {}",
               wb_after_insert - wb_before_insert);

  // Phase 2: Consume past blocks
  double consume_sec = measure([&] {
    // Consume one block at a time per queue. Each BlockTX consumes one block
    // from each queue in parallel.
    std::atomic<bool> more = true;
    while (more.load(std::memory_order_relaxed)) {
      more.store(false, std::memory_order_relaxed);
      PerThreadARC<BlockTX> block_tx(
          db_, cost_model_, background_commit_worker_,
          static_cast<BlockHeight>(num_blocks + 1),
          BlockExecutionMode::kBlockExecutionModeProposal);

      std::vector<size_t> queue_indices;
      queue_indices.reserve(num_queues_);
      for (size_t q = 0; q < num_queues_; q++)
        queue_indices.push_back(q);

      auto consume_fn = [&](size_t q) {
        auto meta_r = db_.read_retrocausal_queue_metadata(queue_keys_[q]);
        if (!meta_r) {
          // Queue doesn't exist or is empty.
          return;
        }
        auto &meta = *meta_r;
        if (meta.live_entries == 0)
          return;

        // Find the next unconsumed block.
        uint64_t next_block =
            meta.consume_state.max_fully_consumed_block_number + 1;
        if (next_block > meta.max_live_block_num)
          return;

        more.store(true, std::memory_order_relaxed);

        CHECK(block_tx.try_acquire_reference());

        InvokeTX tx(block_tx);
        RetrocausalQueueDelta delta;
        // NOLINTNEXTLINE(modernize-make-unique)
        delta.consume.reset(new RetrocausalQueueConsumeDelta{
            .past_block_consume =
                {
                    .max_fully_consumed_block_number = next_block,
                    .partial_consumed_number = 0,
                },
        });
        if (auto ret =
                tx.request_retrocausal_queue(queue_keys_[q], std::move(delta),
                                             std::to_underlying(
                                                 StorageDeltaFlags::None));
            !ret)
          fail(ret.error());
        if (auto ret = tx.sync_balance_locks(); !ret)
          fail(ret.error());
        if (auto ret = tx.commit(); !ret)
          fail(ret.error());
      };

      workers_.for_each_parallel(queue_indices, consume_fn, 1);

      close_block(block_tx.get());
      auto ret = block_tx.get().commit(workers_).and_then(
          [this] { return db_.commit().transform([](uint64_t) {}); });
      if (!ret)
        fail(ret.error());

      CHECK(block_tx.initiate_shutdown());
    }
  });
  size_t wb_after_consume = db_.disk_bytes_written();
  std::println("consume: {} seconds ({} entries/sec)", consume_sec,
               num_entries_ / consume_sec);
  std::println("consume disk_bytes_written: {}",
               wb_after_consume - wb_after_insert);
  std::println("total: {} seconds", insert_sec + consume_sec);

  // Validation: all queues should be empty.
  for (size_t q = 0; q < num_queues_; q++) {
    auto meta_r = db_.read_retrocausal_queue_metadata(queue_keys_[q]);
    CHECK(!meta_r.has_value() && (meta_r.error() == HogDBCode::kNotFound));
  }
  std::println("validation passed: all queues empty");
}

void
RCQBenchmark::run_concurrent()
{
  constexpr size_t kEntriesPerBlock = kRetrocausalQueueBlockEntriesMaxLen;
  size_t num_blocks =
      std::max(bucket_count(num_entries_, num_queues_ * kEntriesPerBlock), 1UL);

  size_t num_consumers = workers_.worker_count() - num_producers_;
  size_t num_threads = num_producers_ + num_consumers;
  info("concurrent benchmark: {}p / {}c, {} queues, {} blocks", num_producers_,
       num_consumers, num_queues_, num_blocks);

  // Shared block state written by main before the start barrier and read by
  // workers after it.  Safe since the pointed-to objects always outlive the
  // barrier phase in which they are accessed.
  PerThreadARC<BlockTX> *block_tx_ptr = nullptr;
  size_t block_start_idx = 0;
  size_t block_end = 0;
  std::vector<std::atomic<size_t>> *committed_counts_ptr = nullptr;
  std::vector<size_t> *entries_per_queue_ptr = nullptr;
  bool running = true;

  std::barrier<> sync(num_threads + 1);
  using hrc = std::chrono::high_resolution_clock;
  std::atomic<int64_t> block_insert_done_ns{0};
  std::atomic<int64_t> block_consume_done_ns{0};
  double total_insert_sec = 0.0;
  double total_consume_sec = 0.0;

  auto atomic_fetch_max = [](std::atomic<int64_t> &a, int64_t val) {
    int64_t prev = a.load(std::memory_order_relaxed);
    while (val > prev &&
           !a.compare_exchange_weak(prev, val, std::memory_order_relaxed))
      ;
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);

  // Producer threads: thread p handles entries p, p+P, p+2P, ...
  for (size_t p = 0; p < num_producers_; p++) {
    threads.emplace_back([&, p] {
      while (true) {
        sync.arrive_and_wait(); // phase 1: wait for start
        bool should_exit = !running;
        if (!should_exit) {
          for (size_t i = block_start_idx + p; i < block_end;
               i += num_producers_) {
            CHECK(block_tx_ptr->try_acquire_reference());

            InvokeTX tx(*block_tx_ptr);
            size_t q = i % num_queues_;
            RetrocausalQueueDelta delta;
            delta.insert.added_entries.push_back(
                make_rcq_entry(static_cast<uint32_t>(i)));
            if (auto ret = tx.request_retrocausal_queue(
                    queue_keys_[q], std::move(delta),
                    std::to_underlying(StorageDeltaFlags::None));
                !ret)
              fail(ret.error());
            if (auto ret = tx.sync_balance_locks(); !ret)
              fail(ret.error());
            if (auto ret = tx.commit(); !ret)
              fail(ret.error());
            committed_counts_ptr->at(q).fetch_add(1, std::memory_order_release);
          }
          atomic_fetch_max(block_insert_done_ns,
                           hrc::now().time_since_epoch().count());
        }
        sync.arrive_and_wait(); // phase 2: signal done
        if (should_exit)
          break;
      }
    });
  }

  // Consumer threads: thread c handles queues c, c+C, c+2C, ...
  for (size_t c = 0; c < num_consumers; c++) {
    threads.emplace_back([&, c] {
      while (true) {
        sync.arrive_and_wait(); // phase 1: wait for start
        bool should_exit = !running;
        if (!should_exit) {
          for (auto q = c; q < num_queues_; q += num_consumers) {
            if ((*entries_per_queue_ptr)[q] == 0)
              continue;

            size_t expected = (*entries_per_queue_ptr)[q];

            // Wait for first entry before polling.
            while (committed_counts_ptr->at(q).load(
                       std::memory_order_acquire) == 0)
              cpu_relax();

            // Collect all entries for this queue in this block.
            std::vector<QueueEntry> collected;
            collected.reserve(expected);
            uint32_t next_index = 0;
            while (collected.size() < expected) {
              auto r =
                  block_tx_ptr->get().get_concurrent_rcq_entry(queue_keys_[q],
                                                               next_index);
              if (!r)
                fail(r.error());
              if (*r) {
                collected.push_back(std::move(**r));
                next_index++;
              }
            }

            CHECK(block_tx_ptr->try_acquire_reference());

            InvokeTX tx(*block_tx_ptr);
            RetrocausalQueueDelta delta;
            auto *consume = new RetrocausalQueueConsumeDelta{};

            auto meta_r = db_.read_retrocausal_queue_metadata(queue_keys_[q]);
            if (meta_r)
              consume->past_block_consume = meta_r->consume_state;

            for (auto &e : collected)
              consume->current_block_consumed.push_back(std::move(e));
            delta.consume.reset(consume);

            if (auto ret = tx.request_retrocausal_queue(
                    queue_keys_[q], std::move(delta),
                    std::to_underlying(StorageDeltaFlags::None));
                !ret)
              fail(ret.error());
            if (auto ret = tx.sync_balance_locks(); !ret)
              fail(ret.error());
            if (auto ret = tx.commit(); !ret)
              fail(ret.error());
          }
          atomic_fetch_max(block_consume_done_ns,
                           hrc::now().time_since_epoch().count());
        }
        sync.arrive_and_wait(); // phase 2: signal done
        if (should_exit)
          break;
      }
    });
  }

  size_t wb_before_concurrent = db_.disk_bytes_written();
  double total_sec = measure([&] {
    for (size_t block = 0; block < num_blocks; block++) {
      PerThreadARC<BlockTX> block_tx(
          db_, cost_model_, background_commit_worker_,
          static_cast<BlockHeight>(block + 1),
          BlockExecutionMode::kBlockExecutionModeProposal);

      size_t cur_block_end =
          std::min(num_entries_,
                   block_start_idx + (num_queues_ * kEntriesPerBlock));

      std::vector<std::atomic<size_t>> committed_counts(num_queues_);
      std::vector<size_t> entries_per_queue(num_queues_, 0);
      for (size_t i = block_start_idx; i < cur_block_end; i++)
        entries_per_queue[i % num_queues_]++;

      // Reset per-block timing and record start before the phase 1 barrier so
      // the barrier provides the necessary happens-before to workers.
      block_insert_done_ns.store(0, std::memory_order_relaxed);
      block_consume_done_ns.store(0, std::memory_order_relaxed);
      int64_t block_start_ns = hrc::now().time_since_epoch().count();

      // Publish block state for workers.
      block_tx_ptr = &block_tx;
      block_end = cur_block_end;
      committed_counts_ptr = &committed_counts;
      entries_per_queue_ptr = &entries_per_queue;

      sync.arrive_and_wait(); // phase 1: start signal
      sync.arrive_and_wait(); // phase 2: wait for workers to finish

      // Accumulate per-phase elapsed time.  The phase 2 barrier ensures all
      // worker writes to block_*_done_ns are visible here.
      total_insert_sec +=
          (block_insert_done_ns.load(std::memory_order_relaxed) -
           block_start_ns) *
          1e-9;
      total_consume_sec +=
          (block_consume_done_ns.load(std::memory_order_relaxed) -
           block_start_ns) *
          1e-9;

      close_block(block_tx.get());
      auto ret = block_tx.get().commit(workers_).and_then(
          [this] { return db_.commit().transform([](uint64_t) {}); });
      if (!ret)
        fail(ret.error());
      info("committed block {}/{}", block, num_blocks);

      CHECK(block_tx.initiate_shutdown());

      block_start_idx = cur_block_end;
    }

    // Signal workers to exit via a final two-phase barrier cycle.
    running = false;
    sync.arrive_and_wait(); // phase 1: exit signal
    sync.arrive_and_wait(); // phase 2: exit done
  });

  for (auto &t : threads)
    t.join();

  size_t wb_after_concurrent = db_.disk_bytes_written();
  std::println("concurrent insert: {} seconds ({} entries/sec)",
               total_insert_sec, num_entries_ / total_insert_sec);
  std::println("concurrent consume: {} seconds ({} entries/sec)",
               total_consume_sec, num_entries_ / total_consume_sec);
  std::println("concurrent insert+consume: {} seconds ({} entries/sec)",
               total_sec, num_entries_ / total_sec);
  std::println("concurrent disk_bytes_written: {}",
               wb_after_concurrent - wb_before_concurrent);

  // Validation: all queues should be empty.
  for (size_t q = 0; q < num_queues_; q++) {
    auto meta_r = db_.read_retrocausal_queue_metadata(queue_keys_[q]);
    CHECK(!meta_r.has_value());
  }
  std::println("validation passed: all queues empty");
}

} // namespace

} // namespace hogd

int
main(int argc, char *argv[]) // NOLINT(bugprone-exception-escape)
{
  int ret = hogd::hogd_init(argc, argv);
  if (ret)
    return ret;
  hogd::info("starting hogdb_bench!");

  auto print_usage = [] {
    std::println("usage: hogdb_bench <db_path> <command> [options]");
    std::println("");
    std::println("options:");
    std::println("  -n, --num <N>          entries (default: 1000000)");
    std::println("  -w, --workers <N>      worker threads (default: 128)");
    std::println("  -q, --queues <N>       RCQ queues (default: 1)");
    std::println("  -p, --producers <N>    rcq_concurrent producer threads "
                 "(default: workers/2)");
    std::println("  -N, --nonces           enable nonces (payment commands)");
    std::println("  -d, --db_hash          compute db hash after execution");
    std::println("  -t, --theta <F>        zipf_payments skew "
                 "(default: 0.99; 0 is uniform)");
    std::println("  -H, --heap <F>         heap provisioning multiplier "
                 "(default: 8.0; lower lets the compactor run)");
    std::println("  -S, --sorted           place the hot payees contiguously "
                 "(consolidated ceiling)");
    std::println("  -h, --help             show this help");
    std::println("");
    std::println("commands:");
    for (const auto &[name, desc, ign] : hogd::benchmarks)
      std::println("  {:24s}{}", name, desc);
    std::println("  {:24s}{}", "rcq", "sequential RCQ insert-then-consume");
    std::println("  {:24s}{}", "rcq_concurrent",
                 "concurrent RCQ insert+consume");
  };

  if (argc < 3) {
    print_usage();
    return 1;
  }

  hogd::db_path = argv[1];
  std::string cmd = argv[2];

  // NOLINTBEGIN(misc-include-cleaner,modernize-use-designated-initializers)
  // clang-format off
  static const struct option long_opts[] = {
      {"num",       required_argument, nullptr, 'n'},
      {"workers",   required_argument, nullptr, 'w'},
      {"queues",    required_argument, nullptr, 'q'},
      {"producers", required_argument, nullptr, 'p'},
      {"nonces",    no_argument,       nullptr, 'N'},
      {"db_hash",   no_argument,       nullptr, 'd'},
      {"theta",     required_argument, nullptr, 't'},
      {"heap",      required_argument, nullptr, 'H'},
      {"sorted",    no_argument,       nullptr, 'S'},
      {"help",      no_argument,       nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  // clang-format on

  size_t n = 1'000'000;
  int workers = 128;
  size_t num_queues = 1;
  int num_producers = -1; // -1 means default to workers/2
  bool use_nonces = false;
  bool db_hash = false;

  // Skip past the two positional args before parsing opts.
  optind = 3;
  int opt;
  while ((opt = getopt_long(argc, argv, "n:w:q:p:Ndht:H:S", long_opts, nullptr)) !=
         -1) {
    switch (opt) {
    case 'n':
      n = std::stoul(optarg, nullptr, 0);
      break;
    case 'w':
      workers = std::stoi(optarg, nullptr, 0);
      break;
    case 'q':
      num_queues = std::stoul(optarg, nullptr, 0);
      break;
    case 'p':
      num_producers = std::stoi(optarg, nullptr, 0);
      break;
    case 'N':
      use_nonces = true;
      break;
    case 'd':
      db_hash = true;
      break;
    case 't':
      hogd::bench_theta = std::stod(optarg);
      break;
    case 'H':
      hogd::bench_heap_factor = std::stod(optarg);
      break;
    case 'S':
      hogd::bench_sorted_hot = true;
      break;
    case 'h':
      print_usage();
      return 0;
    default:
      print_usage();
      return 1;
    }
  }
  // NOLINTEND(misc-include-cleaner,modernize-use-designated-initializers)

  // RCQ benchmarks.
  if (cmd == "rcq" || cmd == "rcq_concurrent") {
    bool concurrent = (cmd == "rcq_concurrent");
    if (num_producers < 0)
      num_producers = workers / 2;
    auto bench = hogd::RCQBenchmark::make(n, workers, num_queues, concurrent,
                                          num_producers, db_hash);
    bench->run();
    return 0;
  }

  for (const auto &[name, ign, fn] : hogd::benchmarks) {
    if (cmd != name)
      continue;

    std::unique_ptr<hogd::Benchmark> bench =
        fn(n, workers, use_nonces, db_hash);
    double sec = hogd::measure([&bench] { bench->run(); });
    std::println("took {} seconds", sec);
    hogd::info("took {} seconds", sec);
    // Report where items landed, once the workload has finished placing them.
    bench->report_layout(cmd);
    return 0;
  }

  std::println("error: unknown command '{}'", cmd);
  print_usage();
  return 1;
}
