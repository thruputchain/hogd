// Scheduler stress tests for parallel scheduling.
//
// These tests are intentionally more intensive than the regular scheduler tests
// and may take longer to run. They are separated into their own file to allow
// faster CI runs while still providing thorough testing for releases.
//
// Test tiers:
// - Medium: ~1K accounts, ~10K txs - suitable for regular CI
// - Large: ~100K accounts, ~1M txs - suitable for nightly/release CI (disabled)

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>

#include "hogd/base/types.h"
#include "hogd/init.h"
#include "hogd/mempool/scheduler.h"
#include "hogd/mempool/test/infra.h"
#include "hogd/mempool/test/txs.h"
#include "types.hh"

namespace hogd {

namespace {

// Helper to create a unique address for stress testing.
ContractAddress make_stress_address(size_t i)
{
  ContractAddress addr{};
  addr[0] = static_cast<uint8_t>(i & 0xFF);
  addr[1] = static_cast<uint8_t>((i >> 8) & 0xFF);
  addr[2] = static_cast<uint8_t>((i >> 16) & 0xFF);
  addr[3] = 0xAA; // Marker byte for stress test accounts
  return addr;
}

} // namespace

// ============================================================================
// Medium Scale Tests (~1K accounts, ~10K txs)
// These should complete in < 30 seconds and are suitable for regular CI.
// ============================================================================

TEST(SchedulerStressMedium, ManyAccounts)
{
  // Test scheduling with many accounts (1K) and many transactions (10K).
  // Verifies that the parallel algorithm handles high account counts correctly.

  constexpr size_t kNumAccounts = 1'000;
  constexpr size_t kTxsPerAccount = 10;
  constexpr size_t kTotalTxs = kNumAccounts * kTxsPerAccount;
  constexpr size_t kRequestedTxs = 5'000;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Create accounts and add transactions
  std::vector<fib_client_t> fib_clients;
  fib_clients.reserve(kNumAccounts);

  for (size_t i = 0; i < kNumAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    // Set up each account with gas - varying ages for fairness testing
    constexpr uint128_t kHogsPerAccount = 100'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i % 1000,
                    kHogsPerAccount);
  }

  // Add transactions in batches to avoid overwhelming the scheduler
  for (size_t i = 0; i < kNumAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  EXPECT_EQ(scheduler->size(), kTotalTxs);

  // Schedule transactions
  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kRequestedTxs, scheduled_hashes, scheduled)();

  // Verify results
  EXPECT_EQ(scheduled.size(), kRequestedTxs)
      << "Should schedule exactly " << kRequestedTxs << " txs";
  EXPECT_EQ(scheduled_hashes.size(), kRequestedTxs);

  // Verify no duplicates
  std::unordered_set<SignedTransactionHash> seen;
  for (const auto &hash : scheduled_hashes) {
    auto [it, inserted] = seen.insert(hash);
    EXPECT_TRUE(inserted) << "Duplicate transaction scheduled";
  }

  // Verify scheduler state is consistent
  EXPECT_EQ(scheduler->size(), kTotalTxs - kRequestedTxs);
}

TEST(SchedulerStressMedium, HotAccounts)
{
  // Test with many transactions from few accounts (hot accounts).
  // This stresses the per-account locking and gas deduction logic.

  constexpr size_t kNumHotAccounts = 10;
  constexpr size_t kTxsPerAccount = 1'000;
  constexpr size_t kTotalTxs = kNumHotAccounts * kTxsPerAccount;
  constexpr size_t kRequestedTxs = 5'000;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  std::vector<fib_client_t> fib_clients;
  for (size_t i = 0; i < kNumHotAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    // Give accounts enough gas for all their transactions
    constexpr uint128_t kHogsPerAccount = 1'000'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i * 10,
                    kHogsPerAccount);
  }

  // Add transactions
  for (size_t i = 0; i < kNumHotAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  EXPECT_EQ(scheduler->size(), kTotalTxs);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kRequestedTxs, scheduled_hashes, scheduled)();

  EXPECT_EQ(scheduled.size(), kRequestedTxs);

  // Verify no duplicates
  std::unordered_set<SignedTransactionHash> seen;
  for (const auto &hash : scheduled_hashes) {
    auto [it, inserted] = seen.insert(hash);
    EXPECT_TRUE(inserted) << "Duplicate transaction scheduled";
  }
}

TEST(SchedulerStressMedium, RepeatedBlocks)
{
  // Test scheduling multiple blocks in sequence.
  // Verifies stability over many scheduling rounds.

  constexpr size_t kNumAccounts = 100;
  constexpr size_t kInitialTxsPerAccount = 50;
  constexpr size_t kBlockSize = 500;
  constexpr size_t kNumBlocks = 10;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  std::vector<fib_client_t> fib_clients;
  for (size_t i = 0; i < kNumAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    constexpr uint128_t kHogsPerAccount = 1'000'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i % 100,
                    kHogsPerAccount);
  }

  // Add initial transactions
  for (size_t i = 0; i < kNumAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kInitialTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  size_t total_scheduled = 0;
  std::unordered_set<SignedTransactionHash> all_scheduled;

  for (size_t block = 0; block < kNumBlocks; block++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;

    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
    scheduler->increment_current_block();

    // May schedule fewer if running out of transactions
    EXPECT_LE(scheduled.size(), kBlockSize);
    total_scheduled += scheduled.size();

    // Verify no duplicates across all blocks
    for (const auto &hash : scheduled_hashes) {
      auto [it, inserted] = all_scheduled.insert(hash);
      EXPECT_TRUE(inserted)
          << "Duplicate transaction scheduled across blocks at block " << block;
    }

    // If we've scheduled everything, break early
    if (scheduler->size() == 0) {
      break;
    }
  }

  // We should have scheduled at most all the transactions
  EXPECT_LE(total_scheduled, kNumAccounts * kInitialTxsPerAccount);
}

TEST(SchedulerStressMedium, ConcurrentAddSchedule)
{
  // Test concurrent add_txs() and schedule() operations.
  // This is a critical test for race conditions.

  constexpr size_t kNumAccounts = 100;
  constexpr size_t kTxsPerAccount = 100;
  constexpr size_t kBlockSize = 500;
  constexpr size_t kNumScheduleRounds = 10;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  // Set up accounts
  std::vector<fib_client_t> fib_clients;
  for (size_t i = 0; i < kNumAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    constexpr uint128_t kHogsPerAccount = 1'000'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i,
                    kHogsPerAccount);
  }

  // Generate all transactions upfront
  std::vector<SignedTransactionBatch> all_txs;
  all_txs.reserve(kNumAccounts);
  for (size_t i = 0; i < kNumAccounts; i++) {
    all_txs.push_back(generate_unsigned_txs(fib_clients[i], kTxsPerAccount));
  }

  // Add half the transactions initially
  for (size_t i = 0; i < kNumAccounts / 2; i++) {
    scheduler->add_txs(all_txs[i])();
  }

  std::atomic<size_t> add_index{kNumAccounts / 2};
  std::atomic<size_t> total_scheduled{0};
  std::atomic<bool> add_done{false};

  // Background thread to add more transactions
  std::thread adder([&]() {
    size_t idx;
    while ((idx = add_index.fetch_add(1, std::memory_order_relaxed)) <
           kNumAccounts) {
      scheduler->add_txs(all_txs[idx])();
    }
    add_done.store(true, std::memory_order_release);
  });

  // Main thread schedules transactions
  std::unordered_set<SignedTransactionHash> all_scheduled;
  for (size_t round = 0; round < kNumScheduleRounds; round++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;

    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
    scheduler->increment_current_block();

    total_scheduled.fetch_add(scheduled.size(), std::memory_order_relaxed);

    for (const auto &hash : scheduled_hashes) {
      auto [it, inserted] = all_scheduled.insert(hash);
      EXPECT_TRUE(inserted)
          << "Duplicate transaction scheduled in concurrent test";
    }
  }

  adder.join();

  // Final verification - no duplicates were scheduled
  EXPECT_EQ(all_scheduled.size(),
            total_scheduled.load(std::memory_order_relaxed));
}

// ============================================================================
// Large Scale Tests (~100K accounts, ~1M txs)
// These may take several minutes and are suitable for nightly/release CI.
// Disabled by default - enable with --gtest_also_run_disabled_tests
// ============================================================================

TEST(DISABLED_SchedulerStressLarge, ManyAccounts)
{
  // Full scale test with 100K accounts and 1M transactions.

  constexpr size_t kNumAccounts = 100'000;
  constexpr size_t kTxsPerAccount = 10;
  constexpr size_t kTotalTxs = kNumAccounts * kTxsPerAccount;
  constexpr size_t kRequestedTxs = 500'000;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  std::vector<fib_client_t> fib_clients;
  fib_clients.reserve(kNumAccounts);

  for (size_t i = 0; i < kNumAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    constexpr uint128_t kHogsPerAccount = 100'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i % 10000,
                    kHogsPerAccount);
  }

  for (size_t i = 0; i < kNumAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  EXPECT_EQ(scheduler->size(), kTotalTxs);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kRequestedTxs, scheduled_hashes, scheduled)();

  EXPECT_EQ(scheduled.size(), kRequestedTxs);

  std::unordered_set<SignedTransactionHash> seen;
  for (const auto &hash : scheduled_hashes) {
    auto [it, inserted] = seen.insert(hash);
    EXPECT_TRUE(inserted);
  }
}

TEST(DISABLED_SchedulerStressLarge, HotAccounts)
{
  // Extreme contention test: 100 accounts with 10K txs each.

  constexpr size_t kNumHotAccounts = 100;
  constexpr size_t kTxsPerAccount = 10'000;
  constexpr size_t kTotalTxs = kNumHotAccounts * kTxsPerAccount;
  constexpr size_t kRequestedTxs = 500'000;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  std::vector<fib_client_t> fib_clients;
  for (size_t i = 0; i < kNumHotAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    constexpr uint128_t kHogsPerAccount = 10'000'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i,
                    kHogsPerAccount);
  }

  for (size_t i = 0; i < kNumHotAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  EXPECT_EQ(scheduler->size(), kTotalTxs);

  x23::XdrVec<SignedTransactionHash> scheduled_hashes;
  x23::XdrVec<SignedTransaction> scheduled;
  scheduler->schedule(kRequestedTxs, scheduled_hashes, scheduled)();

  EXPECT_EQ(scheduled.size(), kRequestedTxs);

  std::unordered_set<SignedTransactionHash> seen;
  for (const auto &hash : scheduled_hashes) {
    auto [it, inserted] = seen.insert(hash);
    EXPECT_TRUE(inserted);
  }
}

TEST(DISABLED_SchedulerStressLarge, RepeatedBlocks)
{
  // Long-running stability test: 10K accounts, 1000 blocks.

  constexpr size_t kNumAccounts = 10'000;
  constexpr size_t kInitialTxsPerAccount = 100;
  constexpr size_t kBlockSize = 1'000;
  constexpr size_t kNumBlocks = 1'000;

  MempoolTestInfra server;
  std::shared_ptr<Scheduler> scheduler = server.mempool_coord_rpc().scheduler();

  std::vector<fib_client_t> fib_clients;
  for (size_t i = 0; i < kNumAccounts; i++) {
    ContractAddress addr = make_stress_address(i);
    fib_clients.emplace_back(addr);

    constexpr uint128_t kHogsPerAccount = 10'000'000'000'000ULL;
    server.set_hogs(addr, kHogAgeGroupToSchedule, /*age=*/i % 1000,
                    kHogsPerAccount);
  }

  for (size_t i = 0; i < kNumAccounts; i++) {
    SignedTransactionBatch txs =
        generate_unsigned_txs(fib_clients[i], kInitialTxsPerAccount);
    scheduler->add_txs(txs)();
  }

  std::unordered_set<SignedTransactionHash> all_scheduled;

  for (size_t block = 0; block < kNumBlocks; block++) {
    x23::XdrVec<SignedTransactionHash> scheduled_hashes;
    x23::XdrVec<SignedTransaction> scheduled;

    scheduler->schedule(kBlockSize, scheduled_hashes, scheduled)();
    scheduler->increment_current_block();

    for (const auto &hash : scheduled_hashes) {
      auto [it, inserted] = all_scheduled.insert(hash);
      EXPECT_TRUE(inserted) << "Duplicate at block " << block;
    }

    if (scheduler->size() == 0) {
      break;
    }
  }
}

} // namespace hogd

int
main(int argc, char *argv[])
{
  auto ret = hogd::hogd_init(argc, argv, false);
  if (ret != 0)
    return ret;
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
