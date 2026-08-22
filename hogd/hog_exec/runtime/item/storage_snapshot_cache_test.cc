#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

#include "contract.hh"
#include "hogd/base/rpc_server.h"
#include "hogd/base/types.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/runtime/item/database_proxy.h"
#include "hogd/hog_exec/runtime/item/storage_snapshot_cache.h"
#include "hogd/hog_exec/runtime/rv32/test_artifact_builder.h"
#include "hogd/hog_exec/runtime/rv32/test_program_builder.h"
#include "hogd/hog_exec/runtime/test_util/block_snapshot_server.h"
#include "rpc_types.hh"
#include "types.hh"

namespace hogd {
namespace {

// A BlockSnapshotRpc that counts how many times the contract bytecode is
// fetched from the database, so tests can observe whether concurrent cold
// lookups coalesce into a single fetch.
class CountingContractRpc final : public BlockSnapshotRpc {
public:
  using BlockSnapshotRpc::BlockSnapshotRpc;

  ExpectedContract get_contract(BlockID block, SHA256Hash const &contract_hash)
  {
    contract_fetches_.fetch_add(1, std::memory_order_relaxed);
    return BlockSnapshotRpc::get_contract(block, contract_hash);
  }

  size_t contract_fetches() const
  {
    return contract_fetches_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<size_t> contract_fetches_{0};
};

// Concurrent cold lookups of the same stored rv32 contract must fetch and
// translate it exactly once: the single-flight coalescing in
// get_contract_entry avoids the duplicated, attacker-triggerable
// database-fetch + rv32-translation work (issue #3543). Without it, each
// contender that observes the cold miss repeats the whole fill.
TEST(StorageSnapshotCacheSingleFlight, ConcurrentColdRv32MissesFetchOnce)
{
  constexpr BlockID kBlock = 4255;
  constexpr size_t kContenders = 8;

  const std::vector<uint32_t> words = {rv32::test_program::addi(1, 0, 1)};
  Contract rv32_contract(ContractType::kRv32);
  rv32_contract.rv32_contract().data.append_range(
      rv32::test_artifact::build_artifact(words, std::vector<uint8_t>(256)));
  const auto contract_hash = sha256(rv32_contract);

  RpcServer<CountingContractRpc> server(kBlock);
  server.start_server(/*port=*/0);
  server.impl()->setup_create_contract(rv32_contract);

  DatabaseProxy db(LocalMockDB{.read_port_number_ = server.port()});
  StorageSnapshotCache cache(kBlock, db);

  std::vector<const Contract *> resolved(kContenders, nullptr);
  std::vector<std::thread> threads;
  threads.reserve(kContenders);
  std::barrier start_gate(static_cast<ptrdiff_t>(kContenders));
  for (size_t i = 0; i < kContenders; ++i) {
    threads.emplace_back([&, i] {
      start_gate.arrive_and_wait();
      auto result = cache.try_get_contract_by_hash(contract_hash);
      if (result.has_value()) {
        resolved[i] = result.value();
      }
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }

  EXPECT_EQ(server.impl()->contract_fetches(), 1U);
  for (const auto *contract : resolved) {
    ASSERT_NE(contract, nullptr);
    EXPECT_EQ(contract, resolved.front());
  }

  // A warm lookup after the fill must not fetch again.
  auto warm = cache.try_get_contract_by_hash(contract_hash);
  ASSERT_TRUE(warm.has_value());
  EXPECT_EQ(warm.value(), resolved.front());
  EXPECT_EQ(server.impl()->contract_fetches(), 1U);
}

} // namespace
} // namespace hogd
