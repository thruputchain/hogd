#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>

#include "built_contracts/output_alias_test.h"
#include "hogd/base/types.h"
#include "hogd/hogexec/precompiles/context.h"
#include "hogd/hogexec/runtime/contract_trait.h"
#include "hogd/hogexec/runtime/contract_trait_client.h"
#include "hogd/hogexec/runtime/native_contract_test_framework.h"
#include "hogd/hogexec/runtime/runtime.h"
#include "rpc_types.hh"
#include "transaction.hh"
#include "types.hh"

namespace hogd {

class OutputAliasTestWasmTest : public ::testing::Test,
                                public NativeContractTestFramework {
public:
  OutputAliasTestWasmTest()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal,
          TestDB::kHogDB),
      wasm_runtime_context_(make_hogexec_context())
  {}

protected:
  void SetUp() override
  {
    using output_alias_test_meta =
        ContractTraitMeta<output_alias_test::OutputAliasTest>;

    auto contract = output_alias_test_meta::load_wasm();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);
  }

  constexpr static ContractAddress kContractAddress = make_contract_address(0);

  ContractTraitClient<output_alias_test::OutputAliasTest> client_ =
      ContractTraitClient<output_alias_test::OutputAliasTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> wasm_runtime_context_;

  uint64_t nonce_ = 0;
};

// Two i128 output pointers at the exact same address must fault.
TEST_F(OutputAliasTestWasmTest, NnintExactOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_nnint_exact_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Two i128 output pointers with partial overlap must fault.
TEST_F(OutputAliasTestWasmTest, NnintPartialOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_nnint_partial_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Non-overlapping outputs should not fault.
TEST_F(OutputAliasTestWasmTest, NnintNoOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_nnint_no_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// output_written_len pointer inside the output span must fault.
TEST_F(OutputAliasTestWasmTest, CalldataSpanWritableOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_calldata_span_writable_overlap">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Non-overlapping span and writable output should not fault.
TEST_F(OutputAliasTestWasmTest, CalldataNoOverlap)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_calldata_no_overlap">(UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Two input pointers at the same address should fault
TEST_F(OutputAliasTestWasmTest, InputInputOverlapFault)
{
  auto handler = make_new_system_call_handler(
      Transaction{
          .invocation =
              client_.make_invocation<"test_log_event_input_input_alias">(
                  UINT64_MAX),
          .uniquifier = Uniquifier{.block_expiry_height =
                                       std::numeric_limits<BlockHeight>::max(),
                                   .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

// Overlapping input with output should fault
TEST_F(OutputAliasTestWasmTest, InputOutputOverlapFault)
{
  auto handler = make_new_system_call_handler(
      Transaction{.invocation =
                      client_.make_invocation<"test_nnint_input_alias_output">(
                          UINT64_MAX),
                  .uniquifier =
                      Uniquifier{.block_expiry_height =
                                     std::numeric_limits<BlockHeight>::max(),
                                 .nonce = nonce_++}},
      *wasm_runtime_context_);
  ASSERT_TRUE(handler.root_invoke().is_root_success());
}

} // namespace hogd
