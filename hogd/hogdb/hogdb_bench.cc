// hogdb_bench.cc - benchmarks for hogdb

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <getopt.h>

#include "hogd/base/log.h"
#include "hogd/base/perthread_arc.h"
#include "hogd/base/types.h"
#include "hogd/base/worker_pool.h"
#include "hogd/base/xdr_util.h"
#include "hogd/container/merkle/merkle_trie.h"
#include "hogd/hogdb/auth_change_subscription_manager.h"
#include "hogd/hogdb/background_commit_worker.h"
#include "hogd/hogdb/block_tx.h"
#include "hogd/hogdb/hog_types.h"
#include "hogd/hogdb/hogdb_dao.h"
#include "hogd/hogdb/invoke_tx.h"
#include "hogd/hogdb/status.h"
#include "hogd/init.h"
#include "hogd/ramdb/db.h"
#include "hogd/storage_cost/storage_cost_model.h"
#include "retrocausal_queue.hh"
#include "rpc_types.hh"
#include "storage_cost.hh"
#include "storage_item.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

namespace {

constexpr int128_t kDefaultBalance = 100;

// config parameter: location to store ramdb.
std::string db_path;

void
fail(HogDBError err)
{
  panic("Unexpected error: {}", err);
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
  make_one_to_many_payments_benchmark(size_t n, int workers, bool use_nonces);
  static std::unique_ptr<Benchmark>
  make_many_to_one_payments_benchmark(size_t n, int workers, bool use_nonces);
  static std::unique_ptr<Benchmark>
  make_round_robin_payments_benchmark(size_t n, int workers, bool use_nonces);
  static std::unique_ptr<Benchmark>
  make_new_storage_benchmark(size_t n, int workers, bool use_nonces);

  Benchmark(HogDBDAO db, std::unique_ptr<MerkleTriePrefix> trie, int workers,
            std::vector<Payment> &&ops)
    : db_(std::move(db)), trie_(std::move(trie)), workers_(workers),
      ops_(std::move(ops))
  {}
  ~Benchmark() = default;

  // run executes the benchmark. This function can be timed directly to measure
  // performance.
  void run();

private:
  HogDBDAO db_;
  std::unique_ptr<MerkleTriePrefix> trie_;
  WorkerPool workers_;
  std::vector<Payment> ops_;
  AuthChangeSubscriptionManager subscription_manager_;

  StorageCostModel cost_model_{StorageCostModel::default_cost_model()};
};

namespace {

constexpr ItemKey
make_key(size_t addr, size_t item)
{
  return make_item_key(make_address(addr), item);
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
  // heap should be large enough to avoid compaction.
  heap_size *= 8;

  int bucket_order = std::countr_zero(std::bit_floor(n));
  HogDBStatus<std::unique_ptr<Database>> db =
      Database::open(db_path, heap_size, bucket_order)
          .transform_error(to_hogdb_error);
  if (!db)
    fail(db.error());
  return HogDBDAO(RAMDB(std::move(*db)));
}

std::unique_ptr<MerkleTriePrefix>
create_merkle()
{
  return std::make_unique<MerkleTriePrefix>();
}

void
populate_account_balances(HogDBDAO &db, size_t n,
                          int128_t val = kDefaultBalance)
{
  for (size_t i = 0; i < n; i++) {
    if (HogDBStatus ret = db.write_address_balance(make_address(i), val); !ret)
      fail(ret.error());
  }
}

} // namespace

std::unique_ptr<Benchmark>
Benchmark::make_one_to_many_payments_benchmark(size_t n, int workers,
                                               bool nonces)
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
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), create_merkle(),
                                           workers, std::move(ops));

  // set up initial DB state.
  HogDBStatus<void> ret;
  ret = bench->db_.write_nnint128(make_key(0, 0), static_cast<int128_t>(n));
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
  HogDBStatus<uint64_t> lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_many_to_one_payments_benchmark(size_t n, int workers,
                                               bool nonces)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n - 1);
  for (size_t i = 1; i < n; i++) {
    ops.emplace_back(make_key(i, 0), make_key(0, 0), 1,
                     maybe_uniquifier(1, i, nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), create_merkle(),
                                           workers, std::move(ops));

  // set up initial DB state.
  HogDBStatus<void> ret;
  ret = bench->db_.write_nnint128(make_key(0, 0), static_cast<int128_t>(0));
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
  HogDBStatus<uint64_t> lsn = bench->db_.commit();
  if (!lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_round_robin_payments_benchmark(size_t n, int workers,
                                               bool use_nonces)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ops.emplace_back(make_key(i, 0), make_key((i + 1) % n, 0), 1,
                     maybe_uniquifier(1, i, use_nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), create_merkle(),
                                           workers, std::move(ops));

  // set up initial DB state.
  for (size_t i = 0; i < n; i++) {
    if (HogDBStatus<void> ret = bench->db_.write_nnint128(make_key(i, 0), 1);
        !ret)
      fail(ret.error());
  }
  // initial storage amount per account should be sufficient for replay cache
  // entries because each source only needs to record a single entry.
  populate_account_balances(bench->db_, n,
                            bench->cost_model_.kBaseItemCost +
                                bench->cost_model_.kReplayCacheCostPerEntry);
  if (HogDBStatus<uint64_t> lsn = bench->db_.commit(); !lsn)
    fail(lsn.error());

  return bench;
}

std::unique_ptr<Benchmark>
Benchmark::make_new_storage_benchmark(size_t n, int workers, bool use_nonces)
{
  // generate payment ops.
  std::vector<Payment> ops;
  ops.reserve(n);
  for (size_t i = 0; i < n; i++) {
    ops.emplace_back(make_key(i, 0), make_key(i, 1), 1,
                     maybe_uniquifier(1, i, use_nonces));
  }

  // create the benchmark.
  auto bench = std::make_unique<Benchmark>(open_ramdb(n), create_merkle(),
                                           workers, std::move(ops));

  // set up initial DB state.
  for (size_t i = 0; i < n; i++) {
    if (HogDBStatus<void> ret = bench->db_.write_nnint128(make_key(i, 0), 1);
        !ret)
      fail(ret.error());
  }
  // initial storage amount per account should be sufficient for replay cache
  // entries because each source only needs to record a single entry.
  populate_account_balances(bench->db_, n,
                            bench->cost_model_.kBaseItemCost +
                                bench->cost_model_.kReplayCacheCostPerEntry);
  if (HogDBStatus<uint64_t> lsn = bench->db_.commit(); !lsn)
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
  PerThreadARC<BlockTX> block(db_, *trie_, cost_model_,
                              background_commit_worker, 1);

  DbTip tip{.height = 0, .id = std::numeric_limits<BlockTXID>::max()};
  if (HogDBStatus<void> ret = db_.write_dbtip(tip); !ret)
    fail(ret.error());

  // invoke_tx simulates DB operations from a contract invocation.
  auto invoke_fn = [&block, &tip](const Payment &p) {
    CHECK(block.try_acquire_reference());

    InvokeTX tx(block);
    if (p.uniquifier) {
      CHECK(p.uniquifier->block_expiry_height >= tip.height);
      HogDBStatus<void> ret = tx.request_consume_uniquifier(
          p.src.get_address(), *p.uniquifier,
          std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
      if (!ret)
        fail(ret.error());
    }
    if (HogDBStatus<void> ret =
            tx.request_nnint128(p.src, -p.amount,
                                std::to_underlying(
                                    StorageDeltaFlags::kStorageDeltaFlagNone));
        !ret)
      fail(ret.error());
    if (HogDBStatus<void> ret =
            tx.request_nnint128(p.dst, p.amount,
                                std::to_underlying(
                                    StorageDeltaFlags::kStorageDeltaFlagNone));
        !ret)
      fail(ret.error());
    if (HogDBStatus<void> ret = tx.sync_balance_locks(); !ret)
      fail(ret.error());
    tx.commit();
    tx.release_blocktx_reference();
  };

  // run invoke_tx in parallel on the workers for all payment ops.
  constexpr size_t kBatchesPerWorker = 4;
  size_t width = ops_.size() / workers_.worker_count() / kBatchesPerWorker;
  workers_.for_each_parallel(ops_, invoke_fn, std::max(width, 1UL));

  // commit changes to the ramdb.
  HogDBStatus<void> ret = block.get().commit(workers_).and_then([this] {
    // force merkle root generation so the cost is included.
    trie_->generate_top_level_hashes();
    std::ignore = trie_->get_root_hash();

    // persist everything to the database.
    return db_.commit().transform([](uint64_t lsn) {});
  });
  if (!ret)
    fail(ret.error());

  CHECK(block.initiate_shutdown());
}

using bench_fn = std::unique_ptr<Benchmark> (*)(size_t n, int workers,
                                                bool use_nonces);

const std::tuple<std::string, std::string, bench_fn> benchmarks[] = {
    {"roundrobin_payments", "each payer pays exactly one other payer",
     Benchmark::make_round_robin_payments_benchmark},
    {"onetomany_payments", "one payer pays everyone else",
     Benchmark::make_one_to_many_payments_benchmark},
    {"manytoone_payments", "everyone pays one payee",
     Benchmark::make_many_to_one_payments_benchmark},
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
                                            size_t num_queues, bool concurrent);

  void run();

private:
  RCQBenchmark(HogDBDAO db, std::unique_ptr<MerkleTriePrefix> trie,
               int num_workers, size_t num_entries, size_t num_queues,
               bool concurrent)
    : db_(std::move(db)), trie_(std::move(trie)),
      workers_(concurrent ? num_workers + static_cast<int>(num_queues)
                          : num_workers),
      num_entries_(num_entries), num_queues_(num_queues),
      concurrent_(concurrent)
  {}

  void run_sequential();
  void run_concurrent();

  HogDBDAO db_;
  std::unique_ptr<MerkleTriePrefix> trie_;
  WorkerPool workers_;
  StorageCostModel cost_model_{StorageCostModel::default_cost_model()};
  AuthChangeSubscriptionManager subscription_manager_;
  BackgroundCommitWorker background_commit_worker_{subscription_manager_,
                                                   nullptr};

  size_t num_entries_;
  size_t num_queues_;
  bool concurrent_;

  std::vector<ItemKey> queue_keys_;
};

std::unique_ptr<RCQBenchmark>
RCQBenchmark::make(size_t num_entries, int num_workers, size_t num_queues,
                   bool concurrent)
{
  // Each queue has its own address. Each RCQ block stores up to 1000
  // entries and is a single DB value, so the number of DB items is
  // proportional to the number of blocks rather than entries.
  // Add headroom for balances and metadata.
  size_t num_blocks =
      (num_entries + num_queues * 1000 - 1) / (num_queues * 1000);
  size_t n = num_queues * (num_blocks + 4) + num_queues;
  // Ensure sufficient heap for large entry payloads.
  n = std::max(n, num_entries / 10);
  auto bench = std::unique_ptr<RCQBenchmark>(
      new RCQBenchmark(open_ramdb(n), create_merkle(), num_workers, num_entries,
                       num_queues, concurrent));

  // Generate queue keys: queue i lives at address i, item slot 0.
  bench->queue_keys_.reserve(num_queues);
  for (size_t i = 0; i < num_queues; i++)
    bench->queue_keys_.push_back(make_key(i, 0));

  // Fund each queue-owning address with generous balance for storage costs.
  // Worst case: all entries in one queue, plus blocks.
  int128_t balance_per_addr =
      (bench->cost_model_.kRCQCostPerEntry +
       bench->cost_model_.kRCQCostPerBlock + bench->cost_model_.kBaseItemCost) *
      static_cast<int128_t>(num_entries + 1000);
  populate_account_balances(bench->db_, num_queues, balance_per_addr);

  // Write initial dbtip.
  DbTip tip{.height = 0, .id = std::numeric_limits<BlockTXID>::max()};
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
  size_t num_blocks = (num_entries_ + num_queues_ * kEntriesPerBlock - 1) /
                      (num_queues_ * kEntriesPerBlock);
  if (num_blocks == 0)
    num_blocks = 1;

  // Phase 1: Insert
  double insert_sec = measure([&] {
    size_t block_start_idx = 0;
    for (size_t block = 0; block < num_blocks; block++) {
      PerThreadARC<BlockTX> block_tx(db_, *trie_, cost_model_,
                                     background_commit_worker_,
                                     static_cast<BlockHeight>(block + 1));

      size_t block_end =
          std::min(num_entries_,
                   block_start_idx + num_queues_ * kEntriesPerBlock);

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
        if (HogDBStatus<void> ret = tx.request_retrocausal_queue(
                queue_keys_[i % num_queues_], delta,
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
            !ret)
          fail(ret.error());
        if (HogDBStatus<void> ret = tx.sync_balance_locks(); !ret)
          fail(ret.error());
        tx.commit();
        tx.release_blocktx_reference();
      };

      constexpr size_t kBatchesPerWorker = 4;
      size_t width =
          work_items.size() / workers_.worker_count() / kBatchesPerWorker;
      workers_.for_each_parallel(work_items, insert_fn, std::max(width, 1UL));

      HogDBStatus<void> ret = block_tx.get().commit(workers_).and_then(
          [this] { return db_.commit().transform([](uint64_t) {}); });
      if (!ret)
        fail(ret.error());
      info("committed block {}/{}", block, num_blocks);

      CHECK(block_tx.initiate_shutdown());

      block_start_idx = block_end;
    }
  });
  std::println("insert: {} seconds ({} entries/sec)", insert_sec,
               num_entries_ / insert_sec);

  // Phase 2: Consume past blocks
  double consume_sec = measure([&] {
    // Consume one block at a time per queue. Each BlockTX consumes one block
    // from each queue in parallel.
    bool more = true;
    while (more) {
      more = false;
      PerThreadARC<BlockTX> block_tx(db_, *trie_, cost_model_,
                                     background_commit_worker_,
                                     BlockHeight(num_blocks + 1));

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

        more = true;

        CHECK(block_tx.try_acquire_reference());

        InvokeTX tx(block_tx);
        RetrocausalQueueDelta delta;
        delta.consume.reset(new RetrocausalQueueConsumeDelta{
            .past_block_consume =
                {
                    .max_fully_consumed_block_number = next_block,
                    .partial_consumed_number = 0,
                },
        });
        if (HogDBStatus<void> ret = tx.request_retrocausal_queue(
                queue_keys_[q], delta,
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
            !ret)
          fail(ret.error());
        if (HogDBStatus<void> ret = tx.sync_balance_locks(); !ret)
          fail(ret.error());
        tx.commit();
        tx.release_blocktx_reference();
      };

      workers_.for_each_parallel(queue_indices, consume_fn, 1);

      HogDBStatus<void> ret = block_tx.get().commit(workers_).and_then(
          [this] { return db_.commit().transform([](uint64_t) {}); });
      if (!ret)
        fail(ret.error());

      CHECK(block_tx.initiate_shutdown());
    }
  });
  std::println("consume: {} seconds ({} entries/sec)", consume_sec,
               num_entries_ / consume_sec);
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
  size_t num_blocks = (num_entries_ + num_queues_ * kEntriesPerBlock - 1) /
                      (num_queues_ * kEntriesPerBlock);
  if (num_blocks == 0)
    num_blocks = 1;

  double total_sec = measure([&] {
    size_t block_start_idx = 0;
    for (size_t block = 0; block < num_blocks; block++) {
      PerThreadARC<BlockTX> block_tx(db_, *trie_, cost_model_,
                                     background_commit_worker_,
                                     static_cast<BlockHeight>(block + 1));

      size_t block_end =
          std::min(num_entries_,
                   block_start_idx + num_queues_ * kEntriesPerBlock);

      // Compute how many entries each queue gets in this block.
      std::vector<size_t> entries_per_queue(num_queues_, 0);
      for (size_t i = block_start_idx; i < block_end; i++)
        entries_per_queue[i % num_queues_]++;

      // Per-queue committed entry counters. Consumers wait on these to
      // avoid polling get_concurrent_rcq_entry before the lock exists.
      std::vector<std::atomic<size_t>> committed_counts(num_queues_);

      // Launch consumers via the worker pool. The pool is sized to
      // num_workers + num_queues to ensure producers can always make
      // progress even while consumers spin.
      std::vector<std::future<void>> consumer_futures;
      consumer_futures.reserve(num_queues_);

      for (size_t q = 0; q < num_queues_; q++) {
        if (entries_per_queue[q] == 0)
          continue;

        consumer_futures.push_back(
            workers_.submit([&, q, expected = entries_per_queue[q]] {
              // Wait until at least one entry is committed for this
              // queue before polling, to avoid racing with lock
              // initialization.
              while (committed_counts[q].load(std::memory_order_acquire) == 0) {
                cpu_relax();
              }

              // Spin-poll for entries from the current block.
              std::vector<QueueEntry> collected;
              collected.reserve(expected);
              uint32_t next_index = 0;
              while (collected.size() < expected) {
                auto r = block_tx.get().get_concurrent_rcq_entry(queue_keys_[q],
                                                                 next_index);
                if (!r)
                  fail(r.error());
                if (*r) {
                  collected.push_back(std::move(**r));
                  next_index++;
                }
              }

              CHECK(block_tx.try_acquire_reference());

              InvokeTX tx(block_tx);
              RetrocausalQueueDelta delta;
              auto *consume = new RetrocausalQueueConsumeDelta{};

              // Read current consume_state from DAO to pass through
              // unchanged.
              auto meta_r = db_.read_retrocausal_queue_metadata(queue_keys_[q]);
              if (meta_r)
                consume->past_block_consume = meta_r->consume_state;

              for (auto &e : collected)
                consume->current_block_consumed.push_back(std::move(e));
              delta.consume.reset(consume);

              if (HogDBStatus<void> ret = tx.request_retrocausal_queue(
                      queue_keys_[q], delta,
                      std::to_underlying(
                          StorageDeltaFlags::kStorageDeltaFlagNone));
                  !ret)
                fail(ret.error());
              if (HogDBStatus<void> ret = tx.sync_balance_locks(); !ret)
                fail(ret.error());
              tx.commit();
              tx.release_blocktx_reference();
            }));
      }

      // Run producers in parallel via the worker pool.
      std::vector<size_t> producer_items;
      producer_items.reserve(block_end - block_start_idx);
      for (size_t i = block_start_idx; i < block_end; i++)
        producer_items.push_back(i);

      auto produce_fn = [&](size_t i) {
        CHECK(block_tx.try_acquire_reference());

        InvokeTX tx(block_tx);
        size_t q = i % num_queues_;
        RetrocausalQueueDelta delta;
        delta.insert.added_entries.push_back(
            make_rcq_entry(static_cast<uint32_t>(i)));
        if (HogDBStatus<void> ret = tx.request_retrocausal_queue(
                queue_keys_[q], delta,
                std::to_underlying(StorageDeltaFlags::kStorageDeltaFlagNone));
            !ret)
          fail(ret.error());
        if (HogDBStatus<void> ret = tx.sync_balance_locks(); !ret)
          fail(ret.error());
        tx.commit();
        committed_counts[q].fetch_add(1, std::memory_order_release);
        tx.release_blocktx_reference();
      };

      constexpr size_t kBatchesPerWorker = 4;
      size_t width =
          producer_items.size() / workers_.worker_count() / kBatchesPerWorker;
      workers_.for_each_parallel(producer_items, produce_fn,
                                 std::max(width, 1UL));

      // Wait for consumers to finish before committing the block.
      for (auto &f : consumer_futures) {
        f.wait();
        f.get();
      }

      HogDBStatus<void> ret = block_tx.get().commit(workers_).and_then(
          [this] { return db_.commit().transform([](uint64_t) {}); });
      if (!ret)
        fail(ret.error());
      info("committed block {}/{}", block, num_blocks);

      CHECK(block_tx.initiate_shutdown());

      block_start_idx = block_end;
    }
  });
  std::println("concurrent insert+consume: {} seconds ({} entries/sec)",
               total_sec, num_entries_ / total_sec);

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
    std::println("  -N, --nonces           enable nonces (payment commands)");
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

  // clang-format off
  static const struct option long_opts[] = {
      {"num",     required_argument, nullptr, 'n'},
      {"workers", required_argument, nullptr, 'w'},
      {"queues",  required_argument, nullptr, 'q'},
      {"nonces",  no_argument,       nullptr, 'N'},
      {"help",    no_argument,       nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };
  // clang-format on

  size_t n = 1'000'000;
  int workers = 128;
  size_t num_queues = 1;
  bool use_nonces = false;

  // Skip past the two positional args before parsing opts.
  optind = 3;
  int opt;
  while ((opt = getopt_long(argc, argv, "n:w:q:Nh", long_opts, nullptr)) !=
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
    case 'N':
      use_nonces = true;
      break;
    case 'h':
      print_usage();
      return 0;
    default:
      print_usage();
      return 1;
    }
  }

  // RCQ benchmarks.
  if (cmd == "rcq" || cmd == "rcq_concurrent") {
    bool concurrent = (cmd == "rcq_concurrent");
    auto bench = hogd::RCQBenchmark::make(n, workers, num_queues, concurrent);
    bench->run();
    return 0;
  }

  for (const auto &[name, ign, fn] : hogd::benchmarks) {
    if (cmd != name)
      continue;

    std::unique_ptr<hogd::Benchmark> bench = fn(n, workers, use_nonces);
    double sec = hogd::measure([&bench] { bench->run(); });
    std::println("took {} seconds", sec);
    hogd::info("took {} seconds", sec);
    return 0;
  }

  std::println("error: unknown command '{}'", cmd);
  print_usage();
  return 1;
}
