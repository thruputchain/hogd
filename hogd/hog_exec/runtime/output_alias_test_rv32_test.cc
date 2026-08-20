#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/output_alias_test.h"
#include "exec_result.hh"
#include "hogd/base/types.h"
#include "hogd/hog_exec/precompiles/context.h"
#include "hogd/hog_exec/runtime/contract_trait.h"
#include "hogd/hog_exec/runtime/contract_trait_client.h"
#include "hogd/hog_exec/runtime/native_contract_test_framework.h"
#include "hogd/hog_exec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class OutputAliasTestRv32Test : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  OutputAliasTestRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDb),
      runtime_context_(make_hog_exec_context())
  {}

protected:
  void SetUp() override
  {
    using output_alias_test_meta =
        ContractTraitMeta<output_alias_test::OutputAliasTest>;

    auto contract = output_alias_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<output_alias_test::OutputAliasTest> client_ =
      ContractTraitClient<output_alias_test::OutputAliasTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;

  uint64_t nonce_ = 0;
};

// Two i128 output pointers at the exact same address must fault.
TEST_F(OutputAliasTestRv32Test, NnintExactOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_nnint_exact_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  handler.enable_transaction_stats();
  ASSERT_TRUE(handler.root_invoke().is_root_success());

  // An alias fault is a fault: it bumps the same counter a parse fault does.
  auto *stats = handler.get_transaction_stats();
  ASSERT_NE(stats, nullptr);
  EXPECT_EQ(stats->syscall_fault_count, 1);
}

// Two i128 output pointers with partial overlap must fault.
TEST_F(OutputAliasTestRv32Test, NnintPartialOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<u8"test_nnint_partial_overlap">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Non-overlapping outputs should not fault.
TEST_F(OutputAliasTestRv32Test, NnintNoOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_nnint_no_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// output_written_len pointer inside the output span must fault.
TEST_F(OutputAliasTestRv32Test, CalldataSpanWritableOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_calldata_span_writable_overlap">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Non-overlapping span and writable output should not fault.
TEST_F(OutputAliasTestRv32Test, CalldataNoOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_calldata_no_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Two input pointers at the same address should fault
TEST_F(OutputAliasTestRv32Test, InputInputOverlapFault)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_log_event_input_input_alias">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Zero-length spans under the half-open alias predicate: strictly inside a
// prior range faults, at its exact edges does not.
TEST_F(OutputAliasTestRv32Test, ZeroLenSpanAliasEdges)
{
  // The non-aliasing arms log real events, so this test commits and emits
  // rather than dropping the handler.
  auto signed_tx = SignedTransaction{
      .tx = Transaction{
          .invocation =
              client_.make_invocation<u8"test_log_event_zero_len_data_alias">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}}};
  auto handler = make_new_system_call_handler(signed_tx, *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
  ASSERT_EQ(handler.try_commit_transaction(), ExecResult::ExecResultOK);
  ASSERT_TRUE(
      handler.emit_events(ExecResult::ExecResultOK,
                          ProposedTransaction{.signed_tx = signed_tx})());
}

// Overlapping input with output should fault
TEST_F(OutputAliasTestRv32Test, InputOutputOverlapFault)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<u8"test_nnint_input_alias_output">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
