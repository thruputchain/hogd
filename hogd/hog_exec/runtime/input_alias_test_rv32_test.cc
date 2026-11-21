#include <gtest/gtest.h>

// End-to-end rv32 coverage of which syscall memory arguments may overlap,
// driven through the input_alias_test contract (local_test_contracts): a real
// guest makes each call with raw argument words and asserts on what it gets
// back. Inputs may share bytes with each other; an output may share bytes
// with nothing.

#include <memory>
#include <utility>

#include "built_contracts/input_alias_test.h"
#include "hogd/base/network_config_params.h"
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

// NOLINTNEXTLINE(misc-use-internal-linkage)
class InputAliasTestRv32Test : public ::testing::Test,
                               public NativeContractTestFramework {
public:
  InputAliasTestRv32Test()
    : NativeContractTestFramework(
          false, BlockExecutionMode::kBlockExecutionModeProposal)
  {}

protected:
  using input_alias_test_meta =
      ContractTraitMeta<input_alias_test::InputAliasTest>;

  void SetUp() override
  {
    auto contract = input_alias_test_meta::load_rv32();
    write_contract(contract);
    write_deployment(kContractAddress, contract);
    setup_grant_storage_token(kContractAddress, 1'000'000'000);

    runtime_context_ = make_hog_exec_context();
  }

  static constexpr ContractAddress kContractAddress = make_contract_address(0);

  // Runs one contract method as a transaction of its own. The method asserts
  // on every syscall result itself, so it returning is the pass; `faults` is
  // how many of its syscalls it expects to fault.
  void expect_method_passes(Invocation invocation, uint16_t faults)
  {
    auto handler = make_new_system_call_handler(
        Transaction{.invocation = std::move(invocation),
                    .uniquifier = make_new_uniquifier()},
        *runtime_context_);
    handler.enable_transaction_stats();
    ASSERT_TRUE(handler.root_invoke()().is_root_success());

    const auto *stats = handler.get_transaction_stats();
    ASSERT_NE(stats, nullptr);
    EXPECT_EQ(stats->syscall_fault_count, faults);
  }

  ContractTraitClient<input_alias_test::InputAliasTest> client_ =
      ContractTraitClient<input_alias_test::InputAliasTest>(kContractAddress);

  std::unique_ptr<RuntimeContext> runtime_context_;
};

// Inputs may share bytes with each other: none of these calls faults.

TEST_F(InputAliasTestRv32Test, InputsAtOneAddress)
{
  expect_method_passes(client_.make_invocation<u8"test_inputs_at_one_address">(
                           kDefaultMaxTxGas),
                       0);
}

TEST_F(InputAliasTestRv32Test, EveryInputAtOneAddress)
{
  expect_method_passes(
      client_.make_invocation<u8"test_every_input_at_one_address">(
          kDefaultMaxTxGas),
      0);
}

TEST_F(InputAliasTestRv32Test, InputsOverlappingInPart)
{
  expect_method_passes(
      client_.make_invocation<u8"test_inputs_overlapping_in_part">(
          kDefaultMaxTxGas),
      0);
}

TEST_F(InputAliasTestRv32Test, InputSpanInsideInput)
{
  expect_method_passes(
      client_.make_invocation<u8"test_input_span_inside_input">(
          kDefaultMaxTxGas),
      0);
}

TEST_F(InputAliasTestRv32Test, ZeroLengthInputSpanInsideInput)
{
  expect_method_passes(
      client_.make_invocation<u8"test_zero_length_input_span_inside_input">(
          kDefaultMaxTxGas),
      0);
}

// An output may share bytes with nothing. The fault counts are the number of
// overlapping layouts each method tries.

TEST_F(InputAliasTestRv32Test, OutputOverlappingInput)
{
  expect_method_passes(
      client_.make_invocation<u8"test_output_overlapping_input">(
          kDefaultMaxTxGas),
      6);
}

TEST_F(InputAliasTestRv32Test, OutputOverlappingOutput)
{
  expect_method_passes(
      client_.make_invocation<u8"test_output_overlapping_output">(
          kDefaultMaxTxGas),
      3);
}

// A zero-length span overlaps a range it lies strictly inside: one fault each
// for the span inside, none for the span at either edge.

TEST_F(InputAliasTestRv32Test, ZeroLengthInputSpanAgainstOutput)
{
  expect_method_passes(
      client_.make_invocation<u8"test_zero_length_input_span_against_output">(
          kDefaultMaxTxGas),
      1);
}

TEST_F(InputAliasTestRv32Test, ZeroLengthOutputSpanAgainstInput)
{
  expect_method_passes(
      client_.make_invocation<u8"test_zero_length_output_span_against_input">(
          kDefaultMaxTxGas),
      1);
}

TEST_F(InputAliasTestRv32Test, TouchingRanges)
{
  expect_method_passes(client_.make_invocation<u8"test_touching_ranges">(
                           kDefaultMaxTxGas),
                       0);
}

} // namespace hogd
