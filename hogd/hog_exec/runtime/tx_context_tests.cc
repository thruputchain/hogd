#include <gtest/gtest.h>

// End-to-end rv32 coverage for the transaction-context syscalls, driven
// through the tx_context_test contract (local_test_contracts):
// GetSourceTransactionHash (no other contract-side coverage), GetBlockHeight,
// and the valid-statistic side of GetTransactionStatistics
// (syscall_misc_tests covers only the invalid statistic).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "built_contracts/tx_context_test.h"
#include "hogd/crypto/crypto.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "hogd/hog_exec/runtime/rv32/rv32_contract.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

// NOLINTNEXTLINE(misc-use-internal-linkage)
class TxContextTests : public ::testing::Test,
                       public NativeContractTestFramework {
public:
  TxContextTests()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb)
  {}

protected:
  using tx_context_test_meta =
      ContractTraitMeta<tx_context_test::TxContextTest>;

  void SetUp() override
  {
    auto contract = tx_context_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  // Deploys a second copy of the contract, for tests whose entrypoint invokes
  // one of the contract's own `internal_*` helpers: the reentrance guard
  // rejects an invoke of a contract already on the invocation stack, so the
  // helper has to run at a different address.
  void deploy_target()
  {
    auto contract = tx_context_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kTargetAddress, contract);
    setup_grant_storage_token(kTargetAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);
  constexpr static ContractAddress kTargetAddress = make_contract_address(1);

  ContractTraitClient<tx_context_test::TxContextTest> client_ =
      ContractTraitClient<tx_context_test::TxContextTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;
};

// The hash reported by GetSourceTransactionHash is the sha256 of the
// XDR-encoded Transaction that was actually submitted.
TEST_F(TxContextTests, GetSourceTxHashMatchesSubmittedTx)
{
  Transaction tx{
      .invocation = client_.make_invocation<u8"get_source_tx_hash">(UINT64_MAX),
      .uniquifier = make_new_uniquifier()};
  SHA256Hash expected = sha256(tx);

  auto handler = make_new_system_call_handler(tx, *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  auto decoded = tx_context_test_meta::decode_return<u8"get_source_tx_hash">(
      std::as_bytes(std::span(output.return_buffer().data(),
                              output.return_buffer().size())),
      false);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_TRUE(std::ranges::equal(*decoded, expected));
}

// The contract-visible block height is the executing block's height: the
// parent tip's height + 1 (see SystemCallHandler::get_block_height).
TEST_F(TxContextTests, GetBlockHeightMatchesExecutingBlock)
{
  BlockHeight expected =
      get_db_tip().value.proposal_header.block_height + 1;

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"get_block_height">(UINT64_MAX),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  auto output = handler.get_most_recent_frame_output();
  ASSERT_EQ(output.status_code(), InvocationFrameStatusCode::Return);
  auto decoded = tx_context_test_meta::decode_return<u8"get_block_height">(
      std::as_bytes(std::span(output.return_buffer().data(),
                              output.return_buffer().size())),
      false);
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, expected);
}

// GetTransactionStatistics(GasRemaining) succeeds; the contract asserts the
// reported value is nonzero mid-execution.
TEST_F(TxContextTests, TxStatsGasRemainingValid)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_tx_stats_gas_remaining">(
                          UINT64_MAX),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// The source tx hash is a per-transaction constant: a child frame observes
// the same value as the root frame (asserted inside the contract).
TEST_F(TxContextTests, SourceTxHashConsistentInChildFrame)
{
  deploy_target();

  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_source_tx_hash_in_child">(
                          UINT64_MAX, kTargetAddress),
                  .uniquifier = make_new_uniquifier()},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
